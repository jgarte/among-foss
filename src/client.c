#include "server.h"
#include "client.h"


/* Get a player ID using the file descriptor. */
int get_pid_by_fd(int fd) {
	for (int pid = 0; pid < NUM_PLAYERS; pid++)
		if (players[pid].fd == fd)
			return pid;

	/* If no player could be found, return a negative player ID. */
	return -1;
}

int alive(struct player *player) {
	return player->state == PLAYER_STATE_ALIVE;
}

void notify_kill(struct player *target) {
	/* TODO: Implement */
}

int kill(struct player *player, struct player *target) {
	if (target->stage != PLAYER_STAGE_MAIN)
		return JSON_KILL_INVALID_PLAYER;

	/* If the player is not the impostor. */
	if (!player->is_impostor || target->is_impostor)
		return JSON_KILL_NOT_IMPOSTOR;

	/* If the target and the player are not in the same room. */
	if (player->location != target->location)
		return JSON_KILL_NOT_IN_ROOM;

	target->state = PLAYER_STATE_DEAD;
	send_json_data(target->fd, JSON_DEATH(JSON_DEATH_KILL));

	notify_kill(target);


	return 0;
}

/* Greet the client and ask them for their name. */
int welcome_client(int fd) {
	send_json_data(fd, JSON_INFO);

	for (int i = 0; i < sizeof(players); i++) {
		if (players[i].fd > 0)
			continue;

		if (state.stage != STAGE_LOBBY)
			broadcast_json(-1, JSON_GAME_STATUS(JSON_GAME_STATUS_IN_PROGRESS));

		players[i].fd = fd;
		players[i].stage = PLAYER_STAGE_NAME;

		printf("Assigned player to ID %d\n", i);

		return 0;
	}

	/* Tell the client that the game is full and close the file descriptor. */
	send_json_data(fd, JSON_GAME_STATUS(JSON_GAME_STATUS_FULL));
	close(fd);

	return -1;
}

/* Clean the player's info. */
void disconnect_client(struct player *player, int should_broadcast) {
	player->fd = -1;

	if (should_broadcast)
		broadcast_json(-1, JSON_PLAYER_STATUS(JSON_PLAYER_STATUS_LEAVE, player->name));

	/* This will check whenever the impostor left or not enough crewmates are left. */
	check_win_condition();

	player->name[0] = '\0';
}

/* Handle client input. */
int handle_input(int fd) {
	char input[INPUT_MAX] = {0};
	char *packet_type;
	struct json_object *parsed_input, *arg;

	int pid = get_pid_by_fd(fd);
	struct player *player = &players[pid];
	int len;

	/* Get the input. */
	len = read(fd, input, INPUT_MAX - 1);

	/* If the client sends an invalid length, disconnect them. */
	if (len < 0 || len >= INPUT_MAX) {
		printf("Read error from player %d\n", pid);
		disconnect_client(player, players[pid].stage != PLAYER_STAGE_NAME);

		return -1;

	}

	/* If the client sends an EOF, disconnect them. */
	if (len == 0) {
		printf("Received EOF from player %d\n", pid);
		disconnect_client(player, players[pid].stage != PLAYER_STAGE_NAME);

		return -2;
	}

	for (size_t i = 0; i < sizeof(input); i++) {
		if (input[i] == '\n' || input[i] == '\r') {
			input[i] = '\0';
			break;
		}
	}

	parsed_input = convert_string_to_json_object(input);
	packet_type = get_type(parsed_input);

	/* Log the client's input. */
	printf("%d: %s\n", pid, input);

	/* If the parsed JSON object is null (indicating that the sent data was
	 * not actually JSON or malformed JSON), return. */
	if (parsed_input == NULL || json_object_get_type(parsed_input) != json_type_object) {
		printf("Player %d sent invalid JSON\n", pid);
		return 0;
	}

	/* Try to handle the specified packet type.
	 * If it does not exist, continue with the execution. */
	if (handle_packet(pid, packet_type, parsed_input))
		return 0;

	/* These are special packets that I won't put in the packet handler. */
	switch(players[pid].stage) {
		case PLAYER_STAGE_NAME:
			if (!is_type(parsed_input, "name"))
				return 0;

			arg = get_argument(parsed_input, "name");

			if (!is_valid_json(arg))
				return 0;

			char *name = (char *) json_object_get_string(arg);

			/* Check if the entered name is valid. */
			if (!is_valid_name(name, fd))
				return 0;

			strcpy(players[pid].name, name);

			if (state.stage == STAGE_LOBBY) {
				players[pid].stage = PLAYER_STAGE_LOBBY;
				broadcast_json(fd, JSON_PLAYER_STATUS(JSON_PLAYER_STATUS_LEAVE, players[pid].name));
			}

			/* Greet the client. */
			send_json_data(players[pid].fd, JSON_GREETING);

			break;

		case PLAYER_STAGE_LOBBY:
			if (is_type(parsed_input, "message")) {
				arg = get_argument(parsed_input, "message");

				if (!is_valid_json(arg))
					return 0;

				char *message = (char *) json_object_get_string(arg);
				int valid = 0;

				for (size_t i = 0; i < strlen(message); i++)
					if (!isspace(message[i]))
						valid = 1;

				struct json_object *args = json_object_new_object();

				json_object_object_add(args, "player", json_object_new_string(players[pid].name));
				json_object_object_add(args, "message", json_object_new_string(message));

				if (valid)
					broadcast_json(fd, JSON_CHAT(message, args));
			} else if (is_type(parsed_input, "command")) {
				struct json_object *command = get_argument(parsed_input, "command");
				struct json_object *arguments = get_argument(parsed_input, "arguments");

				/* If no "command" or "arguments" arguments were given, return. */
				if (command == NULL || arguments == NULL)
					return 0;

				/* If the arguments are not the correct type, return. */
				if (json_object_get_type(command) != json_type_string
						|| json_object_get_type(arguments) != json_type_array)
					return 0;

				int arguments_length = json_object_array_length(arguments);

				char *string_command = (char *) json_object_get_string(command);
				char *string_arguments[arguments_length];

				for (int i = 0; i < arguments_length; i++) {
					struct json_object *argument = json_object_array_get_idx(arguments, i);

					if (argument == NULL || json_object_get_type(argument) != json_type_string)
						continue;

					string_arguments[i] = (char *) json_object_get_string(argument);
				}

				parse_command(pid, string_command, string_arguments);
			}

			break;

		case PLAYER_STAGE_MAIN:
			break;

		case PLAYER_STAGE_WAITING:
			break;
	}

	return 0;
}
