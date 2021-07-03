#include "rtmp_session.hpp"
#include "rtmp_control_handler.hpp"

rtmp_session::rtmp_session(boost::asio::ip::tcp::socket socket, rtmp_server_callbackI* callback) : callback_(callback)
    , hs_(this)
    , ctrl_handler_(this) {
    session_ptr_ = std::make_shared<tcp_session>(std::move(socket), this);
    try_read(__FILE__, __LINE__);
}

rtmp_session::~rtmp_session() {

}

void rtmp_session::try_read(const char* filename, int line) {
    log_infof("try to read, filename:%s, line:%d", filename, line);
    session_ptr_->async_read();
}

data_buffer* rtmp_session::get_recv_buffer() {
    return &recv_buffer_;
}

void rtmp_session::rtmp_send(char* data, int len) {
    log_infof("rtmp send data len:%d", len);
    session_ptr_->async_write(data, len);
    return;
}

void rtmp_session::close() {
    log_infof("rtmp session close....");
    boost::asio::ip::tcp::endpoint ep = session_ptr_->get_remote_endpoint();
    callback_->on_close(ep);
}

void rtmp_session::on_write(int ret_code, size_t sent_size) {
    if ((ret_code != 0) || (sent_size == 0)) {
        log_errorf("write callback code:%d, sent size:%lu", ret_code, sent_size);
        close();
        return;
    }
    log_infof("**** on write callback sent_size:%lu", sent_size);
}

void rtmp_session::on_read(int ret_code, const char* data, size_t data_size) {
    log_infof("on read callback return code:%d, data_size:%lu, recv buffer size:%lu",
        ret_code, data_size, recv_buffer_.data_len());
    if ((ret_code != 0) || (data == nullptr) || (data_size == 0)){
        close();
        return;
    }

    recv_buffer_.append_data(data, data_size);
    int ret = handle_request();
    if (ret < 0) {
        close();
    } else if (ret == RTMP_NEED_READ_MORE) {
        try_read(__FILE__, __LINE__);
    } else {
        log_debugf("handle request ok");
    }
}

int rtmp_session::read_fmt_csid() {
    uint8_t* p = nullptr;

    if (recv_buffer_.require(1)) {
        p = (uint8_t*)recv_buffer_.data();
        log_infof("chunk 1st byte:0x%02x", *p);
        fmt_  = ((*p) >> 6) & 0x3;
        csid_ = (*p) & 0x3f;
        recv_buffer_.consume_data(1);
    } else {
        return RTMP_NEED_READ_MORE;
    }

    log_infof("rtmp chunk fmt:%d, csid:%d", fmt_, csid_);
    if (csid_ == 0) {
        if (recv_buffer_.require(1)) {//need 1 byte
            p = (uint8_t*)recv_buffer_.data();
            recv_buffer_.consume_data(1);
            csid_ = 64 + *p;
        } else {
            return RTMP_NEED_READ_MORE;
        }
    } else if (csid_ == 1) {
        if (recv_buffer_.require(2)) {//need 2 bytes
            p = (uint8_t*)recv_buffer_.data();
            recv_buffer_.consume_data(2);
            csid_ = 64;
            csid_ += *p++;
            csid_ += *p;
        } else {
            return RTMP_NEED_READ_MORE;
        }
    } else {
        log_infof("normal csid:%d", csid_);
    }

    return RTMP_OK;
}

int rtmp_session::read_chunk_stream(CHUNK_STREAM_PTR& cs_ptr) {
    int ret = -1;

    if (!fmt_ready_) {
        ret = read_fmt_csid();
        if (ret != 0) {
            return ret;
        }
        fmt_ready_ = true;
    }

    std::unordered_map<uint8_t, CHUNK_STREAM_PTR>::iterator iter = cs_map_.find(csid_);
    if (iter == cs_map_.end()) {
        cs_ptr = std::make_shared<chunk_stream>(this, fmt_, csid_, chunk_size_);
        cs_map_.insert(std::make_pair(csid_, cs_ptr));
    } else {
        cs_ptr =iter->second;
    }

    ret = cs_ptr->read_message_header(fmt_, csid_);
    if ((ret < RTMP_OK) || (ret == RTMP_NEED_READ_MORE)) {
        return ret;
    } else {
        log_infof("read message header ok");
        cs_ptr->dump_header();
        ret = cs_ptr->read_message_payload();
        if (ret == RTMP_OK) {
            fmt_ready_ = false;
            return ret;
        }
        //cs_ptr->dump_payload();
    }

    return ret;
}

int rtmp_session::send_rtmp_ack(uint32_t size) {
    return ctrl_handler_.send_rtmp_ack(size);
}

int rtmp_session::receive_chunk_stream() {
    CHUNK_STREAM_PTR cs_ptr;
    int ret = -1;

    while(true) {
        //receive fmt+csid | basic header | message header | data
        ret = read_chunk_stream(cs_ptr);
        if ((ret < RTMP_OK) || (ret == RTMP_NEED_READ_MORE)) {
            return ret;
        }

        //check whether chunk stream is ready(data is full)
        if (!cs_ptr || !cs_ptr->is_ready()) {
            if (recv_buffer_.data_len() > 0) {
                continue;
            }
            return RTMP_NEED_READ_MORE;
        }

        log_infof("####### chunk stream is ready ########");
        cs_ptr->dump_header();
        //check whether we need to send rtmp control ack
        (void)send_rtmp_ack(cs_ptr->chunk_data_.data_len());

        if ((cs_ptr->type_id_ >= RTMP_CONTROL_SET_CHUNK_SIZE) && (cs_ptr->type_id_ <= RTMP_CONTROL_SET_PEER_BANDWIDTH)) {
            ret = ctrl_handler_.handle_rtmp_control_message(cs_ptr);
            if (ret < RTMP_OK) {
                return ret;
            }
            cs_ptr->reset();
            if (recv_buffer_.data_len() > 0) {
                continue;
            }
            break;
        } else if ((cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_AMF0) || (cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_AMF3)) {
            std::vector<AMF_ITERM*> amf_vec;
            ret = ctrl_handler_.handle_rtmp_command_message(cs_ptr, amf_vec);
            log_infof("handle_rtmp_command_message return %d", ret);
            if (ret < RTMP_OK) {
                for (auto iter : amf_vec) {
                    AMF_ITERM* temp = iter;
                    delete temp;
                }
                return ret;
            }
            for (auto iter : amf_vec) {
                AMF_ITERM* temp = iter;
                delete temp;
            }
            cs_ptr->reset();
            if (recv_buffer_.data_len() > 0) {
                continue;
            }
            break;
        } else {
            log_infof("#### handle media chunk msg len:%u ####", cs_ptr->msg_len_);

            cs_ptr->reset();
            if (recv_buffer_.data_len() > 0) {
                continue;
            }
            //handle video/audio
        }
        break;
    }

    return ret;
}

int rtmp_session::handle_request() {
    int ret = -1;

    if (session_phase_ == initial_phase) {
        ret = hs_.handle_c0c1();
        if ((ret < 0) || (ret == RTMP_NEED_READ_MORE)) {
            return ret;
        }
        recv_buffer_.reset();//be ready to receive c2;
        log_infof("rtmp session phase become c0c1.");
        ret = hs_.send_s0s1s2();
        session_phase_ = handshake_c2_phase;
        return ret;
    } else if (session_phase_ == handshake_c2_phase) {
        log_infof("start handle c2...");
        ret = hs_.handle_c2();
        if ((ret < 0) || (ret == RTMP_NEED_READ_MORE)){
            return ret;
        }

        log_infof("rtmp session phase become rtmp connect, buffer len:%lu", recv_buffer_.data_len());
        session_phase_ = connect_phase;
        if (recv_buffer_.data_len() == 0) {
            return RTMP_NEED_READ_MORE;
        } else {
            log_infof("start handle rtmp phase:%d", (int)session_phase_);
            ret = receive_chunk_stream();
            if ((ret < 0) || (ret == RTMP_NEED_READ_MORE)) {
                return ret;
            }
        }
    } else if (session_phase_ >= connect_phase) {
        log_infof("start handle rtmp phase:%d", (int)session_phase_);
        ret = receive_chunk_stream();
        if (ret < 0) {
            return ret;
        }

        if (ret == RTMP_OK) {
            ret = RTMP_NEED_READ_MORE;
        }
    }

    return ret;
}