#include "media_stream_manager.hpp"
#include "media_packet.hpp"
#include "logger.hpp"

std::unordered_map<std::string, MEDIA_STREAM_PTR> media_stream_manager::media_streams_map_;

int media_stream_manager::add_player(av_writer_base* writer_p) {
    std::string key_str = writer_p->get_key();
    std::string writerid = writer_p->get_writerid();

    std::unordered_map<std::string, MEDIA_STREAM_PTR>::iterator iter = media_streams_map_.find(key_str);
    if (iter == media_stream_manager::media_streams_map_.end()) {
        MEDIA_STREAM_PTR new_stream_ptr = std::make_shared<media_stream>();

        new_stream_ptr->writer_map_.insert(std::make_pair(writerid, writer_p));
        media_stream_manager::media_streams_map_.insert(std::make_pair(key_str, new_stream_ptr));
        log_infof("add player request:%s in new writer list", key_str.c_str());
        return 1;
    }

    log_infof("add play request:%s", key_str.c_str());
    iter->second->writer_map_.insert(std::make_pair(writerid, writer_p));
    return iter->second->writer_map_.size();
}

void media_stream_manager::remove_player(av_writer_base* writer_p) {
    std::string key_str = writer_p->get_key();
    std::string writerid = writer_p->get_writerid();

    log_infof("remove player key:%s", key_str.c_str());
    auto map_iter = media_streams_map_.find(key_str);
    if (map_iter == media_streams_map_.end()) {
        log_warnf("it's empty when remove player:%s", key_str.c_str());
        return;
    }

    auto writer_iter = map_iter->second->writer_map_.find(writerid);
    if (writer_iter != map_iter->second->writer_map_.end()) {
        map_iter->second->writer_map_.erase(writer_iter);
    }

    
    if (map_iter->second->writer_map_.empty() && !map_iter->second->publisher_exist_) {
        //playlist is empty and the publisher does not exist
        media_streams_map_.erase(map_iter);
        log_infof("delete stream %s for the publisher and players are empty.", key_str.c_str());
    }
    return;
}

MEDIA_STREAM_PTR media_stream_manager::add_publisher(const std::string& stream_key) {
    MEDIA_STREAM_PTR ret_stream_ptr;

    auto iter = media_streams_map_.find(stream_key);
    if (iter == media_streams_map_.end()) {
        ret_stream_ptr = std::make_shared<media_stream>();
        ret_stream_ptr->publisher_exist_ = true;
        ret_stream_ptr->stream_key_ = stream_key;
        media_streams_map_.insert(std::make_pair(stream_key, ret_stream_ptr));
        return ret_stream_ptr;
    }

    ret_stream_ptr = iter->second;
    return ret_stream_ptr;
}

void media_stream_manager::remove_publisher(const std::string& stream_key) {
    auto iter = media_streams_map_.find(stream_key);
    if (iter == media_streams_map_.end()) {
        log_warnf("There is not publish key:%s", stream_key.c_str());
        return;
    }

    log_infof("remove publisher in media stream:%s", stream_key.c_str());
    iter->second->publisher_exist_ = false;
    if (iter->second->writer_map_.empty()) {
        log_infof("delete stream %s for the publisher and players are empty.", stream_key.c_str());
        media_streams_map_.erase(iter);
    }
}

int media_stream_manager::writer_media_packet(MEDIA_PACKET_PTR pkt_ptr) {
    MEDIA_STREAM_PTR stream_ptr = add_publisher(pkt_ptr->key_);

    if (!stream_ptr) {
        log_errorf("fail to get stream key:%s", pkt_ptr->key_.c_str());
        return -1;
    }

    stream_ptr->cache_.insert_packet(pkt_ptr);

    for (auto item : stream_ptr->writer_map_) {
        auto writer = item.second;
        if (!writer->is_inited()) {
            writer->set_init_flag(true);
            log_infof("writer gop cache...");
            stream_ptr->cache_.writer_gop(writer);
        } else {
            writer->write_packet(pkt_ptr);
        }
    }

    return 0;
}