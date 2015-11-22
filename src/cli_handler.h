/*
 * cli_handler.h
 *
 *  Created on: Nov 22, 2015
 *      Author: pchero
 */

#ifndef CLI_HANDLER_H_
#define CLI_HANDLER_H_

#include "asterisk.h"
#include "asterisk/cli.h"

int init_cli_handler(void);
void term_cli_handler(void);

#endif /* CLI_HANDLER_H_ */