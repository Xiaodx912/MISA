#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "spdlog/spdlog.h"
using json = nlohmann::json;

#define LISNUM 10 ///最大连接客户端
#define MYPORT 54321 ///随意的一个(>1024)端口号
#define MAXLEN 65536 ///最大字节接收数
#define MYIP "10.0.8.8" ///这个ip可以被ping到就可以用
using namespace std;

constexpr size_t
HASH_STRING_PIECE(const char* string_piece, size_t hashNum = 0) {
    return *string_piece ? HASH_STRING_PIECE(string_piece + 1,
               (hashNum * 131) + *string_piece)
                         : hashNum;
}
constexpr size_t operator"" _HASH(const char* string_pice, size_t) {
    return HASH_STRING_PIECE(string_pice);
}
size_t
CALC_STRING_HASH(const string& str) {
    return HASH_STRING_PIECE(str.c_str());
} // for string switch

set<int> allClientSocket; /// socket not bind to account
json onlineClientUtoF(json::value_t::object);
json onlineClientFtoU(json::value_t::object);
json usrData(json::value_t::object);
json pendingMsg(json::value_t::object);
vector<int> deletefd; ///存储异常的文件描述符

int exit_flag = 0;

char buffer[MAXLEN] = {};

int socketBind(); /// socket()、bind()

void doEpoll(int& sockfd);

void handleEvents(int& epollfd,
    struct epoll_event* events,
    int& num,
    int& sockfd,
    char* buffer);

void handleAccept(int& epollfd, int& sockfd);

void handleRecv(int& epollfd, int& sockfd, char* buffer);

void handleLogin(json data, int& clientfd, int& epollfd);
void handleReg(json data, int& clientfd, int& epollfd);
void handleMsg(json data, int& clientfd, int& epollfd);
void handleEmailQ(json data, int& clientfd, int& epollfd);
void sendJsonPkg(int& targetfd, json data, int& epollfd);

void addEvent(int& epollfd, int& sockfd, int state);

void deleteEvent(int& epollfd, int sockfd, int state);

void modifyEvent(int& epollfd, int& sockfd, int state);

int main() {
    spdlog::set_level(spdlog::level::debug);
    int sockfd = socketBind();
    if (listen(sockfd, LISNUM) == -1) {
        perror("listen()");
        return 0;
    }
    spdlog::debug("Socket listen OK");
    ifstream user_datai("user_data.json");
    if (user_datai) {
        user_datai >> usrData;
        user_datai.close();
    }
    ifstream msg_datai("msg_data.json");
    if (msg_datai) {
        msg_datai >> pendingMsg;
        msg_datai.close();
    }
    doEpoll(sockfd); //main event loop
    ofstream user_datao("user_data.json");
    user_datao << usrData << endl;
    user_datao.close();
    ofstream msg_datao("msg_data.json");
    msg_datao << pendingMsg << endl;
    msg_datao.close();
    return 0;
}

int socketBind() { /// socket()、bind()
    int sockfd;
    struct sockaddr_in my_addr;
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        exit(1);
    }
    spdlog::debug("Socket prepare OK");
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(MYPORT);
    my_addr.sin_addr.s_addr = inet_addr(MYIP);

    memset(my_addr.sin_zero, 0, sizeof(my_addr.sin_zero));

    if (bind(sockfd, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind()");
        exit(1);
    }
    spdlog::debug("Socket bind OK");
    return sockfd;
}

void doEpoll(int& sockfd) {
    int epollfd = epoll_create(LISNUM); ///创建好一个epoll后会产生一个fd值
    struct epoll_event events[LISNUM];
    spdlog::debug("Epoll init OK");

    int evCount;

    addEvent(
        epollfd,
        sockfd,
        EPOLLIN); ///对sockfd这个连接，我们关心的是是否有客户端要连接他，所以说要将读事件设为关心
    spdlog::info("Main loop start");
    while (exit_flag != 42) { ///持续执行
        evCount = epoll_wait(epollfd, events, LISNUM, -1);
        handleEvents(epollfd, events, evCount, sockfd, buffer); ///对得到的事件进行处理
    }
    close(epollfd);
}

void handleEvents(int& epollfd, struct epoll_event* events, int& num, int& sockfd, char* buffer) {
    int listenfd;
    for (int i = 0; i < num; ++i) {
        listenfd = events[i].data.fd;
        if ((listenfd == sockfd) && (events[i].events & EPOLLIN)) {
            handleAccept(epollfd, sockfd); ///处理客户端连接请求
        } else if (events[i].events & EPOLLIN) {
            handleRecv(epollfd, listenfd, buffer); ///处理客户端发送的信息
        }
    }
}

void handleAccept(int& epollfd, int& sockfd) {
    int clientfd;
    struct sockaddr_in clientaddr;
    socklen_t clientaddrlen = 1;
    if ((clientfd = accept(sockfd, (struct sockaddr*)&clientaddr, &clientaddrlen)) == -1) {
        perror("accept()");
    } else {
        spdlog::debug("New client online");
        allClientSocket.insert(clientfd);
        addEvent(epollfd, clientfd, EPOLLIN); ///处理连接，我们关心这个连接的读事件
    }
}

void handleRecv(int& epollfd, int& sockfd, char* buffer) {
    int len = recv(sockfd, buffer, MAXLEN, 0);
    if (len <= 0) {
        spdlog::debug("Client offline");
        if (len == 0)
            perror("disconnect()");
        else
            perror("recv()");
        allClientSocket.erase(sockfd);
        if (onlineClientFtoU.find(to_string(sockfd)) != onlineClientFtoU.end()) {
            json::string_t username = onlineClientFtoU[to_string(sockfd)];
            onlineClientFtoU.erase(to_string(sockfd));
            onlineClientUtoF.erase(username);
        }
        deleteEvent(epollfd, sockfd, EPOLLIN);
    } else {
        if (buffer[0] == '\0')
            return;
        json data;
        try {
            data = json::parse(buffer);
        } catch (json::parse_error& ex) {
            spdlog::error("json parse err. raw:{}", string(buffer));
            return;
        }
        string msgType = data.value("type", "null");
        spdlog::debug("New msg recv, type {}: {}", msgType, data.dump());
        switch (CALC_STRING_HASH(msgType)) {
        case "login"_HASH:
            handleLogin(data, sockfd, epollfd);
            break;
        case "reg"_HASH:
            handleReg(data, sockfd, epollfd);
            break;
        case "msg"_HASH:
            handleMsg(data, sockfd, epollfd);
            break;
        case "emailQuery"_HASH:
            handleEmailQ(data, sockfd, epollfd);
            break;
        case "_EXIT"_HASH:
            spdlog::warn("recv _EXIT");
            exit_flag = 42;
            break;
        default:
            spdlog::warn("no type!: {}", data.dump());
            break;
        }
        // allSend(buffer, sockfd, epollfd);///成功接收到一个字符串就转发给全部客户端
    }
}

void handleLogin(json data, int& clientfd, int& epollfd) {
    auto username = data.value("username", "");
    auto password = data.value("password", "");
    json reply = { { "type", "login_re" } };
    if (username == "" || password == "") {
        reply["stats"] = "login pkg invalid";
        sendJsonPkg(clientfd, reply, epollfd);
        return;
    }
    auto usrinfo = usrData.find(username);
    if (usrinfo == usrData.end()) {
        reply["stats"] = "no such user";
        sendJsonPkg(clientfd, reply, epollfd);
        return;
    }
    if (usrData[username]["password"] != password) {
        reply["stats"] = "wrong password";
        sendJsonPkg(clientfd, reply, epollfd);
        return;
    }
    reply["stats"] = "OK";
    if (pendingMsg.find(username) != pendingMsg.end()) {
        reply["msg"] = pendingMsg[username];
        pendingMsg.erase(username);
    }
    sendJsonPkg(clientfd, reply, epollfd);
    onlineClientUtoF[username] = clientfd;
    onlineClientFtoU[to_string(clientfd)] = username;
    spdlog::debug("User {} login", username);
}
void handleReg(json data, int& clientfd, int& epollfd) {
    data.erase("type");
    auto username = data.value("username", "");
    auto password = data.value("password", "");
    json reply = { { "type", "reg_re" } };
    if (username == "" || password == "") {
        reply["stats"] = "reg pkg invalid";
        sendJsonPkg(clientfd, reply, epollfd);
        return;
    }
    auto usrinfo = usrData.find(username);
    if (usrinfo != usrData.end()) {
        reply["stats"] = "username used";
        sendJsonPkg(clientfd, reply, epollfd);
        return;
    }
    reply["stats"] = "OK";
    sendJsonPkg(clientfd, reply, epollfd);
    usrData[username] = data;
    spdlog::debug("New user {} reg", username);
}
void handleMsg(json data, int& clientfd, int& epollfd) {
    auto target = data.value("to", "");
    if (target == "")
        return;
    if (onlineClientUtoF.find(target) != onlineClientUtoF.end()) {
        int targetfd = onlineClientUtoF[target];
        sendJsonPkg(targetfd, data, epollfd);
        return;
    }
    if (pendingMsg.find(target) == pendingMsg.end()) {
        pendingMsg[target] = {};
    }
    pendingMsg[target].push_back(data);
}
void handleEmailQ(json data, int& clientfd, int& epollfd) {
    json::array_t qlist = data["data"];
    json qresult(json::value_t::array);
    for (auto& qitem : qlist) {
        auto usrinfo = usrData.find(qitem);
        if (usrinfo != usrData.end()) {
            qresult+={qitem,usrData[qitem].value("email","")};
        }
    }
    json reply={{"type","emailQuery_re"},{"data",qresult}};
    sendJsonPkg(clientfd, reply, epollfd);
}

void sendJsonPkg(int& targetfd, json data, int& epollfd) {
    spdlog::debug("Send json: {}", data.dump());
    //strcpy(buffer,data.dump().c_str());
    if (send(targetfd, data.dump().c_str(), strlen(data.dump().c_str()) + 1, 0) == -1) {
        perror("send()");
        allClientSocket.erase(targetfd);
        if (onlineClientFtoU.find(to_string(targetfd)) != onlineClientFtoU.end()) {
            json::string_t username = onlineClientFtoU[to_string(targetfd)];
            onlineClientFtoU.erase(to_string(targetfd));
            onlineClientUtoF.erase(username);
        }
        deleteEvent(epollfd, targetfd, EPOLLIN);
    }
}

void addEvent(int& epollfd, int& sockfd, int state) {
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = sockfd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);
}

void deleteEvent(int& epollfd, int sockfd, int state) {
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = sockfd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, &ev);
    close(sockfd);
}

void modifyEvent(int& epollfd, int& sockfd, int state) {
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = sockfd;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &ev);
}