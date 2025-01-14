/*
 * Copyright 2019-2020, 2023-2024 NXP.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"

/* FreeRTOS kernel includes */
#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOSConfig.h"
#include <string.h>
#include "mbedtls/base64.h"

#include "cJSON.h"

#include "comms_message_handler_fwupdate_common.h"

#if COMMS_MESSAGE_HANDLER_FWUPDATE_SERVER
#include "comms_message_handler_fwupdate_server.h"
#endif

#include "flash_ica_driver.h"

/*******************************************************************************
 * Variable
 ******************************************************************************/

static sln_comms_fwupdate_job_desc_t *currentFwUpdateJob = NULL;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

static sln_comms_message_status_t processFwUpdateCommonReq(void *clientInstance, cJSON *json);
static sln_comms_message_status_t handleClientFwUpdateStartReq(void *clientInstance, cJSON *json);
static sln_comms_message_status_t processFwUpdateStartReq(void *clientInstance, cJSON *json);
static sln_comms_message_status_t processFwUpdateStateReq(void *clientInstance, cJSON *json);
static sln_comms_message_status_t processFwUpdateCleanReq(void *clientInstance, cJSON *json);

/*******************************************************************************
 * Static Functions
 ******************************************************************************/

/**
 * @brief Process the incoming common FwUpdate message
 *
 * @param clientInstance: The client instance data (received data and connection context)
 * @param json: Incoming payload to be processed
 *
 * @return          sln_comms_message_status_t
 */
static sln_comms_message_status_t processFwUpdateCommonReq(void *clientInstance, cJSON *json)
{
    cJSON *messageType;
    cJSON *messageContents;
    sln_comms_message_status_t status = kComms_Success;

    /* Get the message type */
    messageContents = cJSON_GetObjectItemCaseSensitive(json, "fwupdate_common_message");
    messageType     = cJSON_GetObjectItemCaseSensitive(messageContents, "messageType");

    if (!cJSON_IsNumber(messageType))
    {
        status = kComms_InvalidParameter;
    }

    if (status == kComms_Success)
    {
        /* Switch statement for handling specific Common FwUpdate message */
        switch (messageType->valueint)
        {
            case kCommsFwUpdateStart:
                status = processFwUpdateStartReq(clientInstance, messageContents);
                break;

            case kCommsFwUpdateState:
                status = processFwUpdateStateReq(clientInstance, messageContents);
                break;

            case kCommsFwUpdateClean:
                status = processFwUpdateCleanReq(clientInstance, messageContents);
                break;
        }
    }

    return status;
}

/**
 * @brief Process the incoming FwUpdate State JSON message
 *
 * @param clientInstance: The client instance data (received data and connection context)
 * @param json: Incoming payload to be processed
 *
 * @return          sln_comms_message_status_t
 */
static sln_comms_message_status_t processFwUpdateStateReq(void *clientInstance, cJSON *json)
{
    cJSON *jsonMessage, *jsonFwUpdateReqState, *jsonErrorState;
    sln_comms_message_status_t status = kComms_Success;

    jsonMessage = cJSON_CreateObject();

    if (kComms_Success == status)
    {
        /* TODO: Not implemented yet, need to grab FwUpdate status from descriptor */
        if (NULL != getCurrentFwUpdateJob())
        {
            jsonFwUpdateReqState = cJSON_CreateBool(getCurrentFwUpdateJob()->status);
            cJSON_AddItemToObject(jsonMessage, "status", jsonFwUpdateReqState);
            jsonErrorState = cJSON_CreateNumber(status);

            cJSON_AddItemToObject(jsonMessage, "error", jsonErrorState);
            status = SLN_COMMS_MESSAGE_Send(clientInstance, jsonMessage);
        }
        else
        {
            status = kComms_FailedProcessing;
        }
    }

    /* Free the memory */
    cJSON_Delete(jsonMessage);

    return status;
}

/**
 * @brief Process the incoming FwUpdate Client start request
 *
 * @param clientInstance: The client instance data (received data and connection context)
 * @param json: Incoming payload to be processed
 *
 * @return          sln_comms_message_status_t
 */
static sln_comms_message_status_t handleClientFwUpdateStartReq(void *clientInstance, cJSON *json)
{
    cJSON *jsonMessage, *jsonFwUpdateStartStatus;
    sln_comms_message_status_t status = kComms_Success;
    int32_t fica_status               = SLN_FLASH_NO_ERROR;

    if (kComms_Success == status)
    {
        /* Clear the bit and enable the FWUPDATE */
        fica_status = FICA_clr_comm_flag(FICA_COMM_FWUPDATE_BIT);
        if (fica_status == SLN_FLASH_NO_ERROR)
        {
            configPRINTF(("Success: Cleared FWUPDATE bit in FICA\r\n"));
        }
        else
        {
            status = kComms_FailedProcessing;
        }
    }

    if (kComms_Success == status)
    {
        jsonMessage = cJSON_CreateObject();

        /* Prepare the response message */
        jsonFwUpdateStartStatus = cJSON_CreateNumber(status);
        cJSON_AddItemToObject(jsonMessage, "error", jsonFwUpdateStartStatus);

        status = SLN_COMMS_MESSAGE_Send(clientInstance, jsonMessage);

        /* Free the memory */
        cJSON_Delete(jsonMessage);

        /* Need to start the FwUpdate which requires a reboot into the bootloader */
        if (kComms_Success == status)
        {
            /* reset MCU */
            configPRINTF(("FwUpdate Restart now\r\n"));

            /* give time to logging task to print last logs */
            vTaskDelay(1000);

            NVIC_SystemReset();
        }
    }

    return status;
}

/**
 * @brief Process the incoming Generate FwUpdate start request
 *
 * @param clientInstance: The client instance data (received data and connection context)
 * @param json: Incoming payload to be processed
 *
 * @return          sln_comms_message_status_t
 */
static sln_comms_message_status_t processFwUpdateStartReq(void *clientInstance, cJSON *json)
{
    cJSON *jsonMessage, *jsonFwUpdateStartReq, *jsonFwUpdateStartStatus;
    sln_comms_message_status_t status = kComms_Success;
    int32_t fica_status               = SLN_FLASH_NO_ERROR;
    int32_t imageType = FICA_IMG_TYPE_NONE;

    jsonMessage = cJSON_CreateObject();

    if (kComms_Success == status)
    {
        /* If an FwUpdate start already came in, ignore */
        if (currentFwUpdateJob != NULL)
        {
            status = kComms_AlreadyInProgress;
        }
    }

    if (kComms_Success == status)
    {
        /* Create a new FwUpdate descriptor */
        currentFwUpdateJob = pvPortMalloc(sizeof(sln_comms_fwupdate_job_desc_t));
        if (NULL == currentFwUpdateJob)
        {
            status = kComms_FailedProcessing;
        }
        else
        {
            memset(currentFwUpdateJob, 0, sizeof(sln_comms_fwupdate_job_desc_t));
        }
    }

    /* Parse the Job ID */
    if (kComms_Success == status)
    {
        jsonFwUpdateStartReq = cJSON_GetObjectItemCaseSensitive(json, "job_id");
        if (cJSON_IsString(jsonFwUpdateStartReq) && jsonFwUpdateStartReq->valuestring != NULL)
        {
            configPRINTF(("Found element 'job_id': %s\r\n", jsonFwUpdateStartReq->valuestring));
            currentFwUpdateJob->jobId = (char *)pvPortMalloc(strlen(jsonFwUpdateStartReq->valuestring));

            if (NULL == currentFwUpdateJob->jobId)
            {
                status = kComms_FailedProcessing;
            }
            if (kComms_Success == status)
            {
                memcpy(currentFwUpdateJob->jobId, jsonFwUpdateStartReq->valuestring,
                       strlen(jsonFwUpdateStartReq->valuestring));
            }
        }
        else
        {
            status = kComms_InvalidParameter;
        }
    }

    /* Parse the bank in which the image will be programmed */
    if (kComms_Success == status)
    {
        status = FICA_GetCurAppStartType(&imageType);

        if (SLN_FLASH_NO_ERROR == status)
        {
            if(imageType == FICA_IMG_TYPE_APP_A)
            {
                currentFwUpdateJob->appBankType = FICA_IMG_TYPE_APP_B;
            }
            else
            {
                currentFwUpdateJob->appBankType = FICA_IMG_TYPE_APP_A;
            }
        }
        else
        {
            status = kComms_InvalidParameter;
        }
     }

    /* Parse the signature */
    if (kComms_Success == status)
    {
        jsonFwUpdateStartReq = cJSON_GetObjectItemCaseSensitive(json, "signature");
        if (cJSON_IsString(jsonFwUpdateStartReq) && jsonFwUpdateStartReq->valuestring != NULL)
        {
            configPRINTF(("Found element 'signature': %s\r\n", jsonFwUpdateStartReq->valuestring));

            if (kComms_Success == status)
            {
                uint32_t decodeSize = 0;
                uint32_t mbedStatus = 0;

                /* Decode the signature as it comes encoded based on Ivaldi signing process */
                mbedStatus =
                    mbedtls_base64_decode((unsigned char *)currentFwUpdateJob->signature,
                                          COMMS_FWUPDATE_MAX_SIGNATURE_SIZE,
                                          (size_t *)&decodeSize,
                                          (const unsigned char *)jsonFwUpdateStartReq->valuestring,
                                          strlen(jsonFwUpdateStartReq->valuestring));

                if (mbedStatus != 0)
                {
                    status = kComms_InvalidParameter;
                }
            }
        }
        else
        {
            status = kComms_InvalidParameter;
        }
    }

    /* Parse the image size */
    if (kComms_Success == status)
    {
        jsonFwUpdateStartReq = cJSON_GetObjectItemCaseSensitive(json, "image_size");
        if (cJSON_IsNumber(jsonFwUpdateStartReq))
        {
            uint32_t maxSize     = 0;
            uint32_t fica_status = SLN_FLASH_NO_ERROR;
            configPRINTF(("Found element 'image_size': %i\r\n", jsonFwUpdateStartReq->valueint));

            /* Fail if the image size is smaller than a sector size */
            if( jsonFwUpdateStartReq->valueint < FLASH_SECTOR_SIZE )
            {
                status = kComms_FailedProcessing;
            }

            if (kComms_Success == status)
            {
                fica_status = FICA_get_app_img_max_size(currentFwUpdateJob->appBankType, &maxSize);

                /* Fail if it failed to get the max size */
                if (SLN_FLASH_NO_ERROR != fica_status)
                {
                    status = kComms_FailedProcessing;
                }
            }

            /* Fail if it exceeds the maximum size */
            if (kComms_Success == status)
            {
                if ((jsonFwUpdateStartReq->valueint <= maxSize) && (jsonFwUpdateStartReq->valueint >= 0))
                {
                    currentFwUpdateJob->imageSize = jsonFwUpdateStartReq->valueint;
                }
                else
                {
                    status = kComms_InvalidParameter;
                }
            }
        }
        else
        {
            status = kComms_InvalidParameter;
        }
    }

    /* If we are running from the main app, run the client FwUpdate handle
     * If we are running from the bootloader and FwUpdate server is supported, got to FwUpdate start handle
     */
    if (kComms_Success == status)
    {
        int32_t imageType = FICA_IMG_TYPE_NONE;

        fica_status = FICA_GetImgTypeFromResetISRAddr(&imageType);

        if (SLN_FLASH_NO_ERROR != fica_status)
        {
            status = kComms_FailedProcessing;
        }

        if (kComms_Success == status)
        {
            if ((FICA_IMG_TYPE_APP_A == imageType) || (FICA_IMG_TYPE_APP_B == imageType))
            {
                status = handleClientFwUpdateStartReq(clientInstance, json);
            }
            else
            {
#if COMMS_MESSAGE_HANDLER_FWUPDATE_SERVER
                status = handleServerFwUpdateStartReq(currentFwUpdateJob->appBankType);
#else
                status = kComms_InvalidParameter;
#endif
            }
        }
    }

    if ((kComms_Success != status) && (currentFwUpdateJob != NULL))
    {
        if (kComms_AlreadyInProgress != status)
        {
            vPortFree(currentFwUpdateJob);
            currentFwUpdateJob = NULL;
        }
    }

    if (kComms_Success == status)
    {
        /* Send response */
        jsonFwUpdateStartStatus = cJSON_CreateNumber(status);

        cJSON_AddItemToObject(jsonMessage, "error", jsonFwUpdateStartStatus);

        status = SLN_COMMS_MESSAGE_Send(clientInstance, jsonMessage);

        if (kComms_Success != status)
        {
            configPRINTF(("Failed to send comms message\r\n"));
            vPortFree(currentFwUpdateJob);
        }
    }

    /* Free the memory */
    cJSON_Delete(jsonMessage);

    return status;
}

/**
 * @brief Process the incoming Generate FwUpdate clean request.
 *        Frees the current context in order to prepare for a new connection.
 *
 * @param clientInstance: The client instance data (received data and connection context)
 * @param json: Incoming payload to be processed
 *
 * @return          sln_comms_message_status_t
 */
static sln_comms_message_status_t processFwUpdateCleanReq(void *clientInstance, cJSON *json)
{
    cJSON *jsonMessage, *jsonFwUpdateCleanStatus;
    sln_comms_message_status_t status = kComms_Success;

    if (kComms_Success == status)
    {
        /* Free the existing FwUpdate descriptor */
        if (currentFwUpdateJob != NULL)
        {
            if (currentFwUpdateJob->jobId != NULL)
            {
                vPortFree(currentFwUpdateJob->jobId);
                currentFwUpdateJob->jobId = NULL;
            }
            vPortFree(currentFwUpdateJob);
            currentFwUpdateJob = NULL;
        }
    }

    if (kComms_Success == status)
    {
        jsonMessage = cJSON_CreateObject();

        /* Send response */
        jsonFwUpdateCleanStatus = cJSON_CreateNumber(status);

        cJSON_AddItemToObject(jsonMessage, "error", jsonFwUpdateCleanStatus);

        status = SLN_COMMS_MESSAGE_Send(clientInstance, jsonMessage);

        /* Free the memory */
        cJSON_Delete(jsonMessage);

        if (kComms_Success != status)
        {
            configPRINTF(("Failed to send comms message\r\n"));
        }
    }

    return status;
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

sln_comms_fwupdate_job_desc_t *getCurrentFwUpdateJob(void)
{
    return currentFwUpdateJob;
}

sln_comms_message_status_t processFwUpdateReq(void *clientInstance, cJSON *json)
{
    cJSON *messageType;
    cJSON *messageContents;
    sln_comms_message_status_t status = kComms_Success;

    messageContents = cJSON_GetObjectItemCaseSensitive(json, "fwupdate_message");

    messageType = cJSON_GetObjectItemCaseSensitive(messageContents, "messageType");

    if (!cJSON_IsNumber(messageType))
    {
        status = kComms_InvalidParameter;
    }

    if (status == kComms_Success)
    {
        /* Switch statement to pass to the correct module message handler */
        switch (messageType->valueint)
        {
            case kCommsFwUpdateCommonMsg:
                status = processFwUpdateCommonReq(clientInstance, messageContents);
                break;

#if COMMS_MESSAGE_HANDLER_FWUPDATE_SERVER
            case kCommsFwUpdateServerMsg:
                status = processFwUpdateServerReq(clientInstance, messageContents);
                break;
#endif
            default:
                status = kComms_UnknownMessage;
                break;
        }
    }

    return status;
}
