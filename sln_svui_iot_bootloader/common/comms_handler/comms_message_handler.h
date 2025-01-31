/*
 * Copyright 2019-2020 NXP.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file comms_message_handler.h
 * @brief This file contains the FwUpdate handlers (initialization, data processing hook)
 */

#ifndef COMMS_MESSAGE_HANDLER_H_
#define COMMS_MESSAGE_HANDLER_H_

#include "assert.h"
#include "stdbool.h"
#include "stdint.h"

#include "FreeRTOS.h"
#include "comms_message_handler_cfg.h"
#include "common_connection_handler.h"
#include "cJSON.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

typedef enum _sln_comms_message_status
{
    kComms_Success = 0,
    kComms_FailedParsing,
    kComms_CrcMismatch,
    kComms_FailedProcessing,
    kComms_AlreadyInProgress,
    kComms_UnexpectedMessage,
    kComms_UnknownMessage,
    kComms_InvalidParameter,
    kComms_FailedToTransmit,
} sln_comms_message_status_t;

typedef enum _sln_comms_message_type
{
    kCommsAis = 0,
    kCommsFwUpdate,
} sln_comms_message_type_t;

/*******************************************************************************
 * Function Prototypes
 ******************************************************************************/

/**
 * @brief Initialize the comms message handler
 */
void SLN_COMMS_MESSAGE_Init(sln_common_interface_type_t interfaceType);

/**
 * @brief Send a string representation of the JSON message
 *
 * @param readContext: Incoming context containing the received data and the connection context
 * @param jsonMessage: JSON object to be converted to string and sent
 *
 * @return sln_comms_message_status_t
 */
sln_comms_message_status_t SLN_COMMS_MESSAGE_Send(sln_common_connection_recv_context_t *readContext,
                                                  cJSON *jsonMessage);

/**
 * @brief Process the incoming JSON message
 *
 * @param readContext: Incoming context containing the message to be processed and the connection context
 *
 * @return sln_common_connection_message_status
 */
sln_common_connection_message_status_t SLN_COMMS_MESSAGE_Process(sln_common_connection_recv_context_t *readContext);

#endif /* COMMS_MESSAGE_HANDLER_H_ */
