#pragma once

#include <stdbool.h>
#include "tic80.h"

typedef struct tic_control tic_control;

/**
 * Creates and starts listening on a TCP control socket on localhost:port.
 * Returns NULL on failure.
 */
tic_control* tic_control_create(int port);

/**
 * Polls connections, parses incoming commands, updates input state,
 * and handles screenshot extraction. Called once per frame.
 */
void tic_control_poll(tic_control* ctrl, tic80* tic, tic80_input* input, bool* request_quit);

/**
 * Closes the server and client connections and frees memory.
 */
void tic_control_close(tic_control* ctrl);
