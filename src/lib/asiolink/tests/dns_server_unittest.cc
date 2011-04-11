// Copyright (C) 2011  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <config.h>
#include <gtest/gtest.h>

#include <asio.hpp>
#include <asiolink/io_endpoint.h>
#include <asiolink/io_error.h>
#include <asiolink/udp_server.h>
#include <asiolink/tcp_server.h>
#include <asiolink/dns_answer.h>
#include <asiolink/dns_lookup.h>
#include <string>
#include <csignal>
#include <unistd.h> //for alarm

#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>


/// The following tests focus on stop interface for udp and
/// tcp server, there are lots of things can be shared to test
/// both tcp and udp server, so they are in the same unittest

/// The general work flow for dns server, is that wait for user
/// query, once get one query, we will check the data is valid or
/// not, if it passed, we will try to loop up the question, then
/// compose the answer and finally send it back to user. The server
/// may be stopped at any point during this porcess, so the test strategy
/// is that we define 5 stop point and stop the server at these
/// 5 points, to check whether stop is successful
/// The 5 test points are :
///   Before the server start to run
///   After we get the query and check whether it's valid
///   After we lookup the query
///   After we compoisite the answer
///   After user get the final result.

/// The standard about whether we stop the server successfully or not
/// is based on the fact that if the server is still running, the io
/// service won't quit since it will wait for some asynchronized event for
/// server. So if the io service block function run returns we assume
/// that the server is stopped. To avoid stop interface failure which
/// will block followed tests, using alarm signal to stop the blocking
/// io service
///
/// The whole test context including one server and one client, and
/// five stop checkpoints, we call them ServerStopper exclude the first
/// stop point. Once the unittest fired, the client will send message
/// to server, and the stopper may stop the server at the checkpoint, then
/// we check the client get feedback or not. Since there is no DNS logic
/// involved so the message sending between client and server is plain text
/// And the valid checker, question lookup and answer composition are dummy.

using namespace asiolink;
using namespace asio;
namespace {
static const std::string server_ip = "127.0.0.1";
const int server_port = 5553;
//message client send to udp server, which isn't dns package
//just for simple testing
static const std::string query_message("BIND10 is awesome");

// \brief provide capacity to derived class the ability
// to stop DNSServer at certern point
class ServerStopper {
    public:
        ServerStopper() : server_to_stop_(NULL) {}
        virtual ~ServerStopper(){}

        void setServerToStop(DNSServer* server) {
            server_to_stop_ = server;
        }

        void stopServer() const {
            if (server_to_stop_) {
                server_to_stop_->stop();
            }
        }

    private:
        DNSServer* server_to_stop_;
};

// \brief no check logic at all,just provide a checkpoint to stop the server
class DummyChecker : public SimpleCallback, public ServerStopper {
    public:
        virtual void operator()(const IOMessage&) const {
            stopServer();
        }
};

// \brief no lookup logic at all,just provide a checkpoint to stop the server
class DummyLookup : public DNSLookup, public ServerStopper {
    public:
        void operator()(const IOMessage& io_message,
                isc::dns::MessagePtr message,
                isc::dns::MessagePtr answer_message,
                isc::util::OutputBufferPtr buffer,
                DNSServer* server) const {
            stopServer();
            server->resume(true);
        }
};

// \brief copy the data received from user to the answer part
//  provide checkpoint to stop server
class SimpleAnswer : public DNSAnswer, public ServerStopper {
    public:
        void operator()(const IOMessage& message,
                isc::dns::MessagePtr query_message,
                isc::dns::MessagePtr answer_message,
                isc::util::OutputBufferPtr buffer) const
        {
            //copy what we get from user
            buffer->writeData(message.getData(), message.getDataSize());
            stopServer();
        }

};

// \brief simple client, send one string to server and wait for response
//  in case, server stopped and client cann't get response, there is a timer wait
//  for specified seconds (the value is just a estimate since server process logic is quite
//  simple, and all the intercommunication is local) then cancel the waiting.
class SimpleClient : public ServerStopper {
    public:
    static const size_t MAX_DATA_LEN = 256;
    SimpleClient(asio::io_service& service,
                 unsigned int wait_server_time_out)
    {
        wait_for_response_timer_.reset(new deadline_timer(service));
        received_data_ = new char[MAX_DATA_LEN];
        received_data_len_ = 0;
        wait_server_time_out_ = wait_server_time_out;
    }

    virtual ~SimpleClient() {
        delete [] received_data_;
    }

    void setGetFeedbackCallback(boost::function<void()>& func) {
        get_response_call_back_ = func;
    }

    virtual void sendDataThenWaitForFeedback(const std::string& data)  = 0;
    virtual std::string getReceivedData() const = 0;

    void startTimer() {
        wait_for_response_timer_->cancel();
        wait_for_response_timer_->
            expires_from_now(boost::posix_time::
                             seconds(wait_server_time_out_));
        wait_for_response_timer_->
            async_wait(boost::bind(&SimpleClient::stopWaitingforResponse,
                                   this));
    }

    void cancelTimer() { wait_for_response_timer_->cancel(); }

    void getResponseCallBack(const asio::error_code& error, size_t
                             received_bytes)
    {
        cancelTimer();
        if (!error)
            received_data_len_ = received_bytes;
        if (!get_response_call_back_.empty()) {
            get_response_call_back_();
        }
        stopServer();
    }


    protected:
    virtual void stopWaitingforResponse() = 0;

    boost::shared_ptr<deadline_timer> wait_for_response_timer_;
    char* received_data_;
    size_t received_data_len_;
    boost::function<void()> get_response_call_back_;
    unsigned int wait_server_time_out_;
};



class UDPClient : public SimpleClient {
    public:
    //After 1 seconds without feedback client will stop wait
    static const unsigned int server_time_out = 1;

    UDPClient(asio::io_service& service, const ip::udp::endpoint& server) :
        SimpleClient(service, server_time_out)
    {
        server_ = server;
        socket_.reset(new ip::udp::socket(service));
        socket_->open(ip::udp::v4());
    }


    void sendDataThenWaitForFeedback(const std::string& data) {
        received_data_len_ = 0;
        socket_->send_to(buffer(data.c_str(), data.size() + 1), server_);
        socket_->async_receive_from(buffer(received_data_, MAX_DATA_LEN),
                                    received_from_,
                                    boost::bind(&SimpleClient::
                                                getResponseCallBack, this, _1,
                                                _2));
        startTimer();
    }

    virtual std::string getReceivedData() const {
        return (received_data_len_ == 0 ? std::string("") :
                                std::string(received_data_));
    }

    private:
    void stopWaitingforResponse() {
        socket_->close();
    }

    boost::shared_ptr<ip::udp::socket> socket_;
    ip::udp::endpoint server_;
    ip::udp::endpoint received_from_;
};


class TCPClient : public SimpleClient {
    public:
    // after 2 seconds without feedback client will stop wait,
    // this includes connect, send message and recevice message
    static const unsigned int server_time_out = 2;
    TCPClient(asio::io_service& service, const ip::tcp::endpoint& server)
        : SimpleClient(service, server_time_out)
    {
        server_ = server;
        socket_.reset(new ip::tcp::socket(service));
        socket_->open(ip::tcp::v4());
    }


    virtual void sendDataThenWaitForFeedback(const std::string &data) {
        received_data_len_ = 0;
        data_to_send_ = data;
        data_to_send_len_ = data.size() + 1;
        socket_->async_connect(server_, boost::bind(&TCPClient::connectHandler,
                                                    this, _1));
        startTimer();
    }

    virtual std::string getReceivedData() const {
        return (received_data_len_ == 0 ? std::string("") :
                                std::string(received_data_ + 2));
    }

    private:
    void stopWaitingforResponse() {
        socket_->close();
    }

    void connectHandler(const asio::error_code& error) {
        if (!error) {
            data_to_send_len_ = htons(data_to_send_len_);
            socket_->async_send(buffer(&data_to_send_len_, 2),
                                boost::bind(&TCPClient::sendMessageBodyHandler,
                                            this, _1, _2));
        }
    }

    void sendMessageBodyHandler(const asio::error_code& error,
                                size_t send_bytes)
    {
        if (!error && send_bytes == 2) {
            socket_->async_send(buffer(data_to_send_.c_str(),
                                       data_to_send_.size() + 1),
                    boost::bind(&TCPClient::finishSendHandler, this, _1, _2));
        }
    }

    void finishSendHandler(const asio::error_code& error, size_t send_bytes) {
        if (!error && send_bytes == data_to_send_.size() + 1) {
            socket_->async_receive(buffer(received_data_, MAX_DATA_LEN),
                   boost::bind(&SimpleClient::getResponseCallBack, this, _1,
                               _2));
        }
    }

    boost::shared_ptr<ip::tcp::socket> socket_;
    ip::tcp::endpoint server_;
    std::string data_to_send_;
    uint16_t data_to_send_len_;
};



// \brief provide the context which including two client and
// two server, udp client will only communicate with udp server, same for tcp client
class DNSServerTest : public::testing::Test {
    protected:
        void SetUp() {
            ip::address server_address = ip::address::from_string(server_ip);
            checker_ = new DummyChecker();
            lookup_ = new DummyLookup();
            answer_ = new SimpleAnswer();
            udp_server_ = new UDPServer(service, server_address, server_port,
                    checker_, lookup_, answer_);
            udp_client_ = new UDPClient(service,
                    ip::udp::endpoint(server_address,
                        server_port));
            tcp_server_ = new TCPServer(service, server_address, server_port,
                    checker_, lookup_, answer_);
            tcp_client_ = new TCPClient(service,
                    ip::tcp::endpoint(server_address,
                        server_port));
        }


        void TearDown() {
            udp_server_->stop();
            tcp_server_->stop();
            delete checker_;
            delete lookup_;
            delete answer_;
            delete udp_server_;
            delete udp_client_;
            delete tcp_server_;
            delete tcp_client_;
        }


        void testStopServerByStopper(DNSServer* server, SimpleClient* client,
                ServerStopper* stopper)
        {
            static const unsigned int io_service_time_out = 5;
            io_service_is_time_out = false;
            stopper->setServerToStop(server);
            (*server)();
            client->sendDataThenWaitForFeedback(query_message);
            // Since thread hasn't been introduced into the tool box, using signal
            // to make sure run function will eventually return even server stop
            // failed
            void (*prev_handler)(int) = std::signal(SIGALRM, DNSServerTest::stopIOService);
            alarm(io_service_time_out);
            service.run();
            service.reset();
            //cancel scheduled alarm
            alarm(0);
            std::signal(SIGALRM, prev_handler);
        }


        static void stopIOService(int _no_use_parameter) {
            io_service_is_time_out = true;
            service.stop();
        }

        bool serverStopSucceed() const {
            return (!io_service_is_time_out);
        }

        DummyChecker* checker_;
        DummyLookup*  lookup_;
        SimpleAnswer* answer_;
        UDPServer*    udp_server_;
        UDPClient*    udp_client_;
        TCPClient*    tcp_client_;
        TCPServer*    tcp_server_;

        // To access them in signal handle function, the following
        // variables have to be static.
        static asio::io_service service;
        static bool io_service_is_time_out;
};

bool DNSServerTest::io_service_is_time_out = false;
asio::io_service DNSServerTest::service;

// Test whether server stopped successfully after client get response
// client will send query and start to wait for response, once client
// get response, udp server will be stopped, the io service won't quit
// if udp server doesn't stop successfully.
TEST_F(DNSServerTest, stopUDPServerAfterOneQuery) {
    testStopServerByStopper(udp_server_, udp_client_, udp_client_);
    EXPECT_EQ(query_message, udp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

// Test whether udp server stopped successfully before server start to serve
TEST_F(DNSServerTest, stopUDPServerBeforeItStartServing) {
    udp_server_->stop();
    testStopServerByStopper(udp_server_, udp_client_, udp_client_);
    EXPECT_EQ(std::string(""), udp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}


// Test whether udp server stopped successfully during message check
TEST_F(DNSServerTest, stopUDPServerDuringMessageCheck) {
    testStopServerByStopper(udp_server_, udp_client_, checker_);
    EXPECT_EQ(std::string(""), udp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

// Test whether udp server stopped successfully during query lookup
TEST_F(DNSServerTest, stopUDPServerDuringQueryLookup) {
    testStopServerByStopper(udp_server_, udp_client_, lookup_);
    EXPECT_EQ(std::string(""), udp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

// Test whether udp server stopped successfully during composing answer
TEST_F(DNSServerTest, stopUDPServerDuringPrepareAnswer) {
    testStopServerByStopper(udp_server_, udp_client_, answer_);
    EXPECT_EQ(std::string(""), udp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

static void stopServerManyTimes(DNSServer *server, unsigned int times) {
    for (int i = 0; i < times; ++i) {
        server->stop();
    }
}

// Test whether udp server stop interface can be invoked several times without
// throw any exception
TEST_F(DNSServerTest, stopUDPServeMoreThanOnce) {
    ASSERT_NO_THROW({
        boost::function<void()> stop_server_3_times
            = boost::bind(stopServerManyTimes, udp_server_, 3);
        udp_client_->setGetFeedbackCallback(stop_server_3_times);
        testStopServerByStopper(udp_server_, udp_client_, udp_client_);
        EXPECT_EQ(query_message, udp_client_->getReceivedData());
    });
    EXPECT_TRUE(serverStopSucceed());
}


TEST_F(DNSServerTest, stopTCPServerAfterOneQuery) {
    testStopServerByStopper(tcp_server_, tcp_client_, tcp_client_);
    EXPECT_EQ(query_message, tcp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}


// Test whether tcp server stopped successfully before server start to serve
TEST_F(DNSServerTest, stopTCPServerBeforeItStartServing) {
    tcp_server_->stop();
    testStopServerByStopper(tcp_server_, tcp_client_, tcp_client_);
    EXPECT_EQ(std::string(""), tcp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}


// Test whether tcp server stopped successfully during message check
TEST_F(DNSServerTest, stopTCPServerDuringMessageCheck) {
    testStopServerByStopper(tcp_server_, tcp_client_, checker_);
    EXPECT_EQ(std::string(""), tcp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

// Test whether tcp server stopped successfully during query lookup
TEST_F(DNSServerTest, stopTCPServerDuringQueryLookup) {
    testStopServerByStopper(tcp_server_, tcp_client_, lookup_);
    EXPECT_EQ(std::string(""), tcp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}

// Test whether tcp server stopped successfully during composing answer
TEST_F(DNSServerTest, stopTCPServerDuringPrepareAnswer) {
    testStopServerByStopper(tcp_server_, tcp_client_, answer_);
    EXPECT_EQ(std::string(""), tcp_client_->getReceivedData());
    EXPECT_TRUE(serverStopSucceed());
}


// Test whether tcp server stop interface can be invoked several times without
// throw any exception
TEST_F(DNSServerTest, stopTCPServeMoreThanOnce) {
    ASSERT_NO_THROW({
        boost::function<void()> stop_server_3_times
            = boost::bind(stopServerManyTimes, tcp_server_, 3);
        tcp_client_->setGetFeedbackCallback(stop_server_3_times);
        testStopServerByStopper(tcp_server_, tcp_client_, tcp_client_);
        EXPECT_EQ(query_message, tcp_client_->getReceivedData());
    });
    EXPECT_TRUE(serverStopSucceed());
}

}
