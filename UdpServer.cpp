#include "UdpServer.h"

UdpServer::UdpServer(boost::asio::io_service &io_service, const Config &config)
    : config_(config), service_(io_service),
      usocket_(io_service,
               boost::asio::ip::udp::endpoint(
                   boost::asio::ip::address::from_string(config.ServerAddress),
                   config.ServerPort)),
      resolver_(io_service) {}

void UdpServer::run() { doReceive(); }

void UdpServer::doReceive() {
    auto self = shared_from_this();
    usocket_.async_receive_from(
        boost::asio::buffer(buf, sizeof(buf)), endpoint_,
        [this, self](boost::system::error_code ec, std::size_t length) {
            handleReceive(ec, length);
        });
}

void UdpServer::handleReceive(boost::system::error_code ec,
                              std::size_t length) {
    if (ec) {
        usocket_.cancel();
        return;
    }
    auto dec = getDecrypter(config_.Method, config_.Password);
    auto ivlen = dec->getIvLen();
    char *data = buf;
    if (length <= ivlen) {
        doReceive();
        return;
    }
    dec->initIV(std::string(data, ivlen));
    data += ivlen;
    length -= ivlen;
    std::string plain = dec->decrypt(std::string(data, length));
    std::copy_n(plain.begin(), plain.length(), std::begin(buf));
    data = buf;
    if (length <= 2) {
        doReceive();
        return;
    }
    auto atyp = data[0];
    data++;
    length--;
    auto self = shared_from_this();
    if (atyp == typeIPv4) {
        if (length <= lenIPv4 + 2) {
            doReceive();
            return;
        }
        auto ep = boost::asio::ip::udp::endpoint(
            boost::asio::ip::address_v4(ntohl(*reinterpret_cast<long *>(data))),
            ntohs(*reinterpret_cast<uint16_t *>(data + lenIPv4 - 2)));
        data += lenIPv4;
        length -= lenIPv4;
        //        if(ep.address().to_string() == "8.8.8.8") {
        //            ep =
        //            boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::from_string("223.6.6.6"),
        //            ep.port());
        //        }
        sendDataFromLocal(ep, std::string(data - lenIPv4 - 1, lenIPv4 + 1),
                          data, length);
    } else if (atyp == typeIPv6) {
        if (length <= lenIPv6) {
            doReceive();
            return;
        }
        boost::asio::ip::address_v6::bytes_type bytes;
        std::copy_n(data, bytes.size(), bytes.data());
        auto ep = boost::asio::ip::udp::endpoint(
            boost::asio::ip::address_v6(bytes),
            ntohs(*reinterpret_cast<uint16_t *>(data + lenIPv6 - 2)));
        data += lenIPv6;
        length -= lenIPv6;
        sendDataFromLocal(ep, std::string(data - lenIPv6 - 1, lenIPv6 + 1),
                          data, length);
    } else if (atyp == typeDm) {
        unsigned char len = data[0];
        data++;
        length--;
        if (length <= len + 2) {
            doReceive();
            return;
        }
        std::string name(data, len);
        data += len;
        length -= len;
        uint16_t port = ntohs(*reinterpret_cast<uint16_t *>(data));
        boost::asio::ip::udp::resolver::query query(name, std::to_string(port));
        data += 2;
        length -= 2;
        std::string header(data - len - 3, len + 3);
        auto self = shared_from_this();
        resolver_.async_resolve(
            query, [this, self, data, length,
                    header](boost::system::error_code ec,
                            boost::asio::ip::udp::resolver::iterator iterator) {
                if (ec) {
                    doReceive();
                    return;
                }
                auto ep = *iterator;
                sendDataFromLocal(ep, header, data, length);
            });
    } else {
        doReceive();
        return;
    }
}

void UdpServer::sendDataFromLocal(boost::asio::ip::udp::endpoint ep,
                                  std::string header, char *data,
                                  std::size_t len) {
    auto self = shared_from_this();
    BOOST_LOG_TRIVIAL(info) << "[udp] " << ep.address().to_string() << ":"
                            << ep.port();
    auto it = sessions_.find(endpoint_);
    if (it == sessions_.end()) {
        sessions_.emplace(endpoint_,
                          UdpSession(boost::asio::ip::udp::socket(
                              service_, boost::asio::ip::udp::v4())));
        recvDataFromRemote(endpoint_);
        it = sessions_.find(endpoint_);
    }
    headers_.emplace(ep, std::move(header));
    auto &session = it->second;
    auto &usocket = session.usocket_;
    usocket.async_send_to(
        boost::asio::buffer(data, len), ep,
        [this, self](boost::system::error_code ec, std::size_t length) {
            doReceive();
            return;
        });
}

void UdpServer::doRecvDataFromRemote(boost::asio::ip::udp::endpoint ep,
                                     boost::system::error_code ec,
                                     std::size_t length) {
    if (ec) {
        return;
    }
    auto self = shared_from_this();
    auto enc = getEncrypter(config_.Method, config_.Password);
    auto iv = enc->getIV();
    auto ivlen = enc->getIvLen();
    auto it = sessions_.find(ep);
    auto &session = it->second;
    char *rbuf = session.rbuf;
    auto dstHeader = enc->encrypt(headers_[session.endpoint_]);
    auto dstBody = enc->encrypt(std::string(rbuf, length));
    std::copy_n(iv.begin(), ivlen, rbuf);
    std::copy_n(dstHeader.begin(), dstHeader.length(), rbuf + ivlen);
    std::copy_n(dstBody.begin(), dstBody.length(),
                rbuf + ivlen + dstHeader.length());
    ivlen += dstHeader.length() + dstBody.length();

    usocket_.async_send_to(
        boost::asio::buffer(rbuf, ivlen), ep,
        [this, self, ep](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                return;
            }
            recvDataFromRemote(ep);
        });
}

void UdpServer::recvDataFromRemote(boost::asio::ip::udp::endpoint ep) {
    auto self = shared_from_this();
    auto it = sessions_.find(ep);
    auto &session = it->second;
    session.usocket_.async_receive_from(
        boost::asio::buffer(session.rbuf, sizeof(session.rbuf)),
        session.endpoint_,
        [this, self, ep](boost::system::error_code ec, std::size_t length) {
            doRecvDataFromRemote(ep, ec, length);
        });
}