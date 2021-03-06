#include "utils.h"
#include "config.h"
#include "encrypt.h"

void printVersion() {
    printf("shadowsocks-asio 0.0.1\n");
    std::exit(0);
}

bool checkDaemon() {
    if (!FLAGS_daemon) {
        return false;
    }
    if (FLAGS_log.empty()) {
        printf("You should set the log file\n");
    }
    return true;
}

bool checkAddress(std::string address) {
    auto it = ForbiddenIPAddresses.find(address);
    if (it == ForbiddenIPAddresses.end()) {
        return true;
    } else {
        return false;
    }
}

void runAfter(asio::io_service &io_service,
              asio::high_resolution_timer::duration td, functor f) {
    auto dt = std::make_shared<asio::high_resolution_timer>(io_service, td);
    dt->async_wait([dt, f](const std::error_code &) { f(); });
}

std::size_t getRandomNumber() {
    static std::random_device rd;
    return rd();
}

void plusOneSecond(asio::io_service &service,
                   asio::ip::tcp::socket &&s) {
    std::size_t n = 0;
    std::size_t x = 0;
    do {
        x = getRandomNumber() % 16 + 1;
        n += x;
    } while (x < 3 || x > 14);
    VLOG(1) << "续了" << n << "秒";
    auto socket = std::make_shared<asio::ip::tcp::socket>(std::move(s));
    runAfter(service, std::chrono::seconds(n), [socket] {});
}
