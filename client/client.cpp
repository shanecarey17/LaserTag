#include <iostream>
#include <string>
#include <set>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "client.hpp"
#include "geometry.hpp"

using namespace Protocol;
using namespace Geometry;

LaserTagClient::LaserTagClient(boost::asio::io_service &io_service, std::string hostname, std::string service_id) 
    : socket_(io_service, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0)),
      timeout_timer_(io_service),
      send_timer_(io_service),
      laser_timer_(io_service),
      laser_available_timer_(io_service),
      players_(std::map<int, Player>()),
      seq_num_(0),
      laser_available_(true) {
    // Resolve server endpoint
    boost::asio::ip::udp::resolver resolver(io_service);
    boost::asio::ip::udp::resolver::query query(boost::asio::ip::udp::v4(), hostname, service_id);
    endpoint_ = *resolver.resolve(query);

    // Request to enter game at server
    RequestEnterGame();
}

std::map<int, Player> &LaserTagClient::Players() {
    return players_;
}

std::pair<int, int> LaserTagClient::GetScore() {
    return std::pair<int, int>(red_score_, blue_score_);
}

int LaserTagClient::GetPlayerNum() {
    return my_player_num_;
}

void LaserTagClient::UpdateState(Input input) {
    // Lock data
    mutex_.lock();

    // Handle input
    switch (input) {
        case (Up) : {
            MyPlayer().MoveForward();
            break;
        }
        case (Down) : {
            MyPlayer().MoveBackward();
            break;    
        }
        case (Left) : {
            MyPlayer().RotateLeft();
            break;
        }
        case (Right) : {
            MyPlayer().RotateRight();
            break;
        }
        case (Space) : {
            Laser();
        }
        default:
            break;
    }

    // Unlock
    mutex_.unlock();
}

void LaserTagClient::RequestEnterGame() {
    // Create and send request packet to server
    std::shared_ptr<ClientDataHeader> request(new ClientDataHeader());
    request->request = true;
    socket_.async_send_to(boost::asio::buffer(request.get(), sizeof(ClientDataHeader)), endpoint_, 
            boost::bind(&LaserTagClient::OnRequestEnterGame, this, _1, _2, request));
}

void LaserTagClient::OnRequestEnterGame(const boost::system::error_code &error, size_t bytes_transferred, std::shared_ptr<ClientDataHeader> request) {
    // Begin receiving game data with initial flag set
    ReceiveGameData(true);

    // Set a timer to re-request entry to the game if we time out
    timeout_timer_.expires_from_now(boost::posix_time::seconds(1));
    timeout_timer_.async_wait(boost::bind(&LaserTagClient::OnEnterGameTimeout, this, _1));
}

void LaserTagClient::OnEnterGameTimeout(const boost::system::error_code &error) {
    if (error) {
        // Timer was cancelled i.e. game data was received
        return;
    }

    // Try to enter the game again
    RequestEnterGame();
}

void LaserTagClient::ReceiveGameData(bool initial) {
    // Receive game data in form of header data and vector of player states
    std::shared_ptr<ServerDataHeader> header(new ServerDataHeader());
    std::shared_ptr<std::vector<TransmittedData>> data(new std::vector<TransmittedData>(32));
    boost::array<boost::asio::mutable_buffer, 2> buffer = {boost::asio::buffer(header.get(), sizeof(ServerDataHeader)), boost::asio::buffer(*data)};
    
    // Special work to do if this is the initial receiving of data
    if (initial) {
        socket_.async_receive_from(buffer, endpoint_, boost::bind(&LaserTagClient::OnReceiveInitialGameData, this, _1, _2, header, data));
    } else {
        socket_.async_receive_from(buffer, endpoint_, boost::bind(&LaserTagClient::OnReceiveGameData, this, _1, _2, header, data));
    }
}

void LaserTagClient::OnReceiveInitialGameData(const boost::system::error_code &error, size_t bytes_transmitted,
        std::shared_ptr<ServerDataHeader> transmitted_data_header, std::shared_ptr<std::vector<TransmittedData>> transmitted_data) {
    // Cancel timer after we receive game data
    timeout_timer_.cancel();

    // Get the sequence number of server
    last_server_seq_num_ = transmitted_data_header->server_seq_num;

    // Get our data
    my_player_num_ = transmitted_data_header->client_player_num;
    
    // Receive data as usual
    OnReceiveGameData(error, bytes_transmitted, transmitted_data_header, transmitted_data);
    
    // Begin sending current data
    send_timer_.expires_from_now(boost::posix_time::milliseconds(50));
    send_timer_.async_wait(boost::bind(&LaserTagClient::SendPlayerData, this, _1));
}

void LaserTagClient::OnReceiveGameData(const boost::system::error_code &error, size_t bytes_transmitted, 
        std::shared_ptr<ServerDataHeader> transmitted_data_header, std::shared_ptr<std::vector<TransmittedData>> transmitted_data) {
    // Check sequence number of header is in the correct order
    if (transmitted_data_header->server_seq_num > last_server_seq_num_) {
    
        // Update header variables
        last_server_seq_num_ = transmitted_data_header->server_seq_num;
        red_score_ = transmitted_data_header->red_score;
        blue_score_ = transmitted_data_header->blue_score;

        // Fetch data from buffer into our map
        transmitted_data->resize(transmitted_data_header->num_players);
        std::set<int> active_players;
        for (TransmittedData player_data : *transmitted_data) {
            InsertOrUpdatePlayer(player_data.player_num, player_data);
            active_players.insert(player_data.player_num);
        }

        // Remove inactive players
        for (auto iter = players_.begin(); iter != players_.end(); /* */) {
            if (active_players.find(iter->first) == active_players.end()) {
                players_.erase(iter++);
            } else {
                iter++;
            }
        }
    }

    // Receive next
    ReceiveGameData(false);
}

void LaserTagClient::InsertOrUpdatePlayer(int player_num, TransmittedData &data) {
    auto iter = players_.find(player_num);
    if (iter == players_.end()) {
        players_.insert(std::make_pair(player_num, Player(data)));
    } else {
        if (player_num == my_player_num_) {
            // If we are updating ourselves, only change to server coordinates if we moved a long distance (i.e. respawned)
            Vector2D new_pos(data.x_pos, data.y_pos);
            Vector2D cur_pos = iter->second.Position();
            if (Norm(new_pos - cur_pos) > 25) {
                iter->second.Update(data);
            }
        } else {
            iter->second.Update(data);
        }
    }
}

void LaserTagClient::SendPlayerData(const boost::system::error_code &error) {
    // Create packet
    std::shared_ptr<ClientDataHeader> header(new ClientDataHeader());
    header->request = false;
    header->seq_num = seq_num_++;
    std::shared_ptr<TransmittedData> data(new TransmittedData(MyPlayer().Data()));
    boost::array<boost::asio::const_buffer, 2> buffer = {boost::asio::buffer(header.get(), sizeof(ClientDataHeader)), boost::asio::buffer(data.get(), sizeof(TransmittedData))};

    // Send asynchronously
    socket_.async_send_to(buffer, endpoint_, boost::bind(&LaserTagClient::OnSendPlayerData, this, _1, _2, header,data));
}

void LaserTagClient::OnSendPlayerData(const boost::system::error_code &error, size_t bytes_transmitted, std::shared_ptr<ClientDataHeader> header, std::shared_ptr<TransmittedData> data) {
    // Timer for the next send
    send_timer_.expires_from_now(boost::posix_time::milliseconds(50));
    send_timer_.async_wait(boost::bind(&LaserTagClient::SendPlayerData, this, _1));
}

void LaserTagClient::Laser() {
    // Only fire laser if available (prevent spamming)
    if (laser_available_ == false) {
        return;
    }

    MyPlayer().SetLaser(true);
    laser_available_ = false;
    
    // Laser fires for a quarter second
    laser_timer_.expires_from_now(boost::posix_time::milliseconds(250));
    laser_timer_.async_wait([this](const boost::system::error_code &error) {
        this->MyPlayer().SetLaser(false);
    });

    // Laser is ready to fire again in 1 seconds
    laser_available_timer_.expires_from_now(boost::posix_time::seconds(1));
    laser_available_timer_.async_wait([this](const boost::system::error_code &error) {
        this->laser_available_ = true;
    });
}

Player &LaserTagClient::MyPlayer() {
    return players_.find(my_player_num_)->second;
}
