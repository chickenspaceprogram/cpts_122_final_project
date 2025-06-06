#include <socket-handling/fd-utils.hpp>
#include <game/game-in-progress.hpp>
#include <algorithm>
#include "parsing.hpp"


static const std::string CONN_ERR("A player encountered a connection error.");
static const std::string INTERNAL_ERR("An unexpected server error occurred.");
static const std::string MSG_ERR("A player sent an invalid message.");

std::optional<std::size_t> find_cardholder(const std::string &search_cards, const std::vector<Player> &players, std::size_t current_player_index) {
    std::size_t str_start = 0;
    std::string_view search_cards_view(search_cards);
    for (int i = 0; i < 3; ++i) {
        // parsing for card
        std::size_t next_comma = search_cards_view.find_first_of(",\r", str_start);
        std::string_view next_card(search_cards_view.substr(str_start, next_comma));
        str_start = next_comma + 1;

        // finding card
        for (std::size_t i = 0; i < players.size(); ++i) {
            if (i == current_player_index) {
                continue;
            }
            const std::vector<std::string> &cards = players[i].cards;
            if (std::find(cards.begin(), cards.end(), next_card) != cards.end()) {
                return std::optional(i);
            }
        }
    }
    return std::optional<std::size_t>(std::nullopt);
}


// This logic is kinda a dumpster fire tbh
// There's so many error conditions that excessive early returns are the only option if I don't want extreme nesting
// either way the code is unreadable lol
//
// it does hopefully work*, which is the important bit!
//
// better error handling in the lower levels of this application (or using a different architecture altogether, classic event-loop servers are kinda annoying) would probably have fixed this



GameInProgress::GameInProgress(GameStartup &&startup) : game(std::move(startup.get_gamedata())) {
    epoll_event ev { .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data = { .fd = -1 } };
    for (Player &player : game.get_players()) {
        ev.data.fd = player.fd;
        game.mod_poll_fd(player.fd, ev);
    }
    std::string playerlist{"PLAYERS-LIST,"};
    for (Player &player : game.get_players()) {
        playerlist += player.name + ',';
    }
    playerlist.pop_back();
    broadcast(playerlist + "\r\nTURN-START," + game.get_players()[0].name + "\r\n");
    is_valid = flush_out();
    if (!is_valid) {
        send_err_msg(CONN_ERR);
    }
}

bool GameInProgress::callback() {
    std::cerr << "woo callback" << std::endl;
    epoll_event ev = game.wait_for_event(-1);
    Player &player_with_event = find_io(ev.data.fd);
    if ((ev.events & EPOLLRDHUP) || (ev.events & EPOLLERR) || (ev.events & EPOLLHUP)) {
        std::cerr << "wtf error" << std::endl;
        send_err_msg(CONN_ERR);
        return false;
    }
    if (ev.events & EPOLLOUT) {
        SocketStatus result(player_with_event.outbuf.flush(player_with_event.fd));
        std::cerr << "wtf" << std::endl;
        switch (result) {
        case SocketStatus::Finished:
        case SocketStatus::Blocked:
            break;
        case SocketStatus::Error:
        case SocketStatus::ZeroReturned:
        std::cerr << "wtf error" << std::endl;
            send_err_msg(CONN_ERR);
            return false;
        }
    }

    if (ev.events & EPOLLIN) {
        std::cerr << "does it get here" << std::endl;

        if (ev.data.fd == game.get_players()[turn_index].fd) {
            return current_player_msg();
        }
        else {
            std::cerr << "goin to other player" << std::endl;
            return other_player_msg(ev.data.fd);
        }
    }
    return true;
}

void GameInProgress::broadcast(const std::string &message) {
    std::shared_ptr<std::string> msg(new std::string(message));
    for (Player &player : game.get_players()) {
        player.outbuf.add_message(msg);
    }
}

bool GameInProgress::flush_out() {
    bool can_continue = true;
    for (Player &player : game.get_players()) {
        switch (player.outbuf.flush(player.fd)) {
        case SocketStatus::Blocked:
        case SocketStatus::Finished:
            break;
        case SocketStatus::Error:
        case SocketStatus::ZeroReturned:
            can_continue = true;
        }
    }
    return can_continue;
}

bool GameInProgress::other_player_msg(int fd) {
    Player &player = find_io(fd);
    switch (player.inbuf.buf_read(player.fd)) {
    case SocketStatus::Error:
    case SocketStatus::ZeroReturned:
        send_err_msg(CONN_ERR);
        return false;
    default:
        break;
    }

    std::optional<std::size_t> msg_end = player.inbuf.get_msg_end();
    if (!msg_end.has_value()) {
        if (player.inbuf.full()) {
            send_err_msg(MSG_ERR);
            return false; // sent too many chars
        }
        else {
            return true; // we've read all we can
        }
    }

    // now we have a valid message!
    auto msg = parse_have_card_msg(player.inbuf, msg_end.value());
    if (!msg.has_value()) {
        send_err_msg(MSG_ERR);
        return false;
    }
    SocketStatus status = SocketStatus::ZeroReturned;
    for (Player &player : game.get_players()) {
        if (player.name == msg.value().first) {
            player.outbuf.add_message(
            // msg is of form "HAVE-CARD,target-playername,card name\r\n"
                std::shared_ptr<std::string>(new std::string(
                    "HAVE-CARD," + msg.value().second + "\r\n"
                ))
            );
            status = player.outbuf.flush(player.fd);
            break;
        }
    }
    player.inbuf.pop_front(msg_end.value());
    switch (status) {
    case SocketStatus::Error:
        send_err_msg(CONN_ERR);
        return false;
        break;
    case SocketStatus::ZeroReturned:
        send_err_msg(MSG_ERR);
        return false;
        break;
    case SocketStatus::Finished:
    case SocketStatus::Blocked:
        break;
    }
    return true;
}

bool GameInProgress::current_player_msg() {  
    std::cerr << "asdf" << std::endl;
    Player &current = game.get_players()[turn_index];
    switch (current.inbuf.buf_read(current.fd)) {
    case SocketStatus::Error:
    case SocketStatus::ZeroReturned:
        send_err_msg(CONN_ERR);
        return false;
    case SocketStatus::Blocked:
    case SocketStatus::Finished:
        break;
    }

    std::optional<std::size_t> msg(current.inbuf.get_msg_end());
    if (!msg.has_value()) {
        return true; // message hasn't fully sent yet
    }

    // now we definitely have something to parse, what is it?
    //

    // bug, never gets here. why?
    std::cerr << "pian" << std::endl;

    std::cerr << current.name << std::endl;
    if (check_cards_msg("CARD-REQUEST,", current.inbuf, msg.value()) && turn_state == HasNotRequestedCards) {
        turn_state = HasRequestedCards;
        std::string result(parse_cards("CARD-REQUEST,", current.inbuf, msg.value()));
        current.inbuf.pop_front(msg.value());
        std::optional<std::size_t> index(find_cardholder(result, game.get_players(), turn_index));
        if (!index.has_value()) {
            // player guessed the right cards (or all their cards)
            broadcast("CARD-REQUEST-EMPTY," + current.name + ',' + result + "\r\n");
            return flush_out();
        }
        Player &found_player = game.get_players()[index.value()];
        broadcast("CARD-REQUEST," + current.name + ',' + found_player.name + ',' + result + "\r\n");
        return flush_out();
    }
    else if (check_cards_msg("ACCUSE,", current.inbuf, msg.value())) {
        if (!handle_accuse(msg.value())) {
            // someone accused successfully (or an error occurred)
            return false;
        }
        current.inbuf.pop_front(msg.value());
        return true; // gotta wait for END-TURN still
    }
    else if (check_endturn_msg(current.inbuf, msg.value())) {
        current.inbuf.pop_front(msg.value());
        // can deref the optional here because the check has already happened in the accuse
        ++turn_index;
        turn_index = search_for_players().value();
        turn_state = HasNotRequestedCards;
        broadcast("TURN-START," + game.get_players()[turn_index].name + "\r\n");
        return flush_out();
    }
    else {
        send_err_msg(MSG_ERR);
        return false;
    }
};


std::optional<std::size_t> GameInProgress::search_for_players() const {
    for (std::size_t i = 0; i < game.get_players().size(); ++i) {
        if (players_in_game[(i + turn_index) % game.get_players().size()]) {
            return std::optional((i + turn_index) % game.get_players().size());
        }
    }
    return std::optional<std::size_t>(std::nullopt);
}
bool GameInProgress::handle_accuse(std::size_t msg_end) {
    Player &current = game.get_players()[turn_index];

    std::string result = parse_cards("ACCUSE,", current.inbuf, msg_end);
    if (!find_cardholder(result, game.get_players(), 1234).has_value()) {
        send_ending_msg(current.name + ",Successful accusation!");
        return false; // game is done
    }

    // we could find someone with the card the player guessed
    broadcast("ACCUSE-FAIL," + current.name + ',' + result + "\r\n");
    if (!flush_out()) {
        send_err_msg(CONN_ERR);
        return false;
    }
    players_in_game[turn_index] = false;
    std::optional<std::size_t> search_res(search_for_players());
    if (!search_res.has_value()) {
        send_ending_msg("All players have made a false accusation.");
        return false;
    }

    return true;
}
