/*
 * Copyright (c) 2015 - 2016, Freescale Semiconductor, Inc.
 * Copyright 2016 - 2022 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"

#include "usb_device_class.h"
#include "usb_device_msc.h"
#include "usb_device_ch9.h"
#include "usb_device_descriptor.h"
#include "disk.h"

#include "fsl_device_registers.h"
#include "clock_config.h"
#include "board.h"
#include "fsl_debug_console.h"
#include "bootloader.h"
#include "sln_push_buttons_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if (defined(FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT > 0U))
#include "fsl_sysmpu.h"
#endif /* FSL_FEATURE_SOC_SYSMPU_COUNT */

#if ((defined FSL_FEATURE_SOC_USBPHY_COUNT) && (FSL_FEATURE_SOC_USBPHY_COUNT > 0U))
#include "usb_phy.h"
#endif

#include "pin_mux.h"
#include "flash_ica_driver.h"
#if ENABLE_ENCRYPTION
#include "nor_encrypt_bee.h"
#endif /* ENABLE_ENCRYPTION */

#include "sln_rgb_led_driver.h"
#include "sln_msc_vfs.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define BLINK_RATE 5

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
void USB_DeviceClockInit(void);
void USB_DeviceIsrEnable(void);
#if USB_DEVICE_CONFIG_USE_TASK
void USB_DeviceTaskFn(void *deviceHandle);
#endif

/*******************************************************************************
 * Variables
 ******************************************************************************/

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
usb_device_inquiry_data_fromat_struct_t g_InquiryInfo = {
    (USB_DEVICE_MSC_UFI_PERIPHERAL_QUALIFIER << USB_DEVICE_MSC_UFI_PERIPHERAL_QUALIFIER_SHIFT) |
        USB_DEVICE_MSC_UFI_PERIPHERAL_DEVICE_TYPE,
    (uint8_t)(USB_DEVICE_MSC_UFI_REMOVABLE_MEDIUM_BIT << USB_DEVICE_MSC_UFI_REMOVABLE_MEDIUM_BIT_SHIFT),
    USB_DEVICE_MSC_UFI_VERSIONS,
    0x02,
    USB_DEVICE_MSC_UFI_ADDITIONAL_LENGTH,
    {0x00, 0x00, 0x00},
    {'N', 'X', 'P', ' ', 'S', 'E', 'M', 'I'},
    {'N', 'X', 'P', ' ', 'M', 'A', 'S', 'S', ' ', 'S', 'T', 'O', 'R', 'A', 'G', 'E'},
    {'0', '0', '0', '1'}};
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
usb_device_mode_parameters_header_struct_t g_ModeParametersHeader = {
    /*refer to ufi spec mode parameter header*/
    0x0000, /*!< Mode Data Length*/
    0x00,   /*!<Default medium type (current mounted medium type)*/
    0x00,   /*!MODE SENSE command, a Write Protected bit of zero indicates the medium is write enabled*/
    {0x00, 0x00, 0x00, 0x00} /*!<This bit should be set to zero*/
};
/* Data structure of msc device, store the information ,such as class handle */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_StorageDisk[DISK_PHYSICAL_SIZE_NORMAL];
usb_msc_struct_t g_msc;
static bool s_fallback = false;

/*******************************************************************************
 * Code
 ******************************************************************************/

void USB_OTG1_IRQHandler(void)
{
    USB_DeviceEhciIsrFunction(g_msc.deviceHandle);
    /* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F Store immediate overlapping
    exception return operation might vector to incorrect interrupt */
    __DSB();
}

void USB_OTG2_IRQHandler(void)
{
    USB_DeviceEhciIsrFunction(g_msc.deviceHandle);
    /* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F Store immediate overlapping
    exception return operation might vector to incorrect interrupt */
    __DSB();
}

void USB_DeviceClockInit(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };
    if (CONTROLLER_ID == kUSB_ControllerEhci0)
    {
        CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, BOARD_USB_CLOCK_FREQ);
        CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, BOARD_USB_CLOCK_FREQ);
    }
    else
    {
        CLOCK_EnableUsbhs1PhyPllClock(kCLOCK_Usbphy480M, BOARD_USB_CLOCK_FREQ);
        CLOCK_EnableUsbhs1Clock(kCLOCK_Usb480M, BOARD_USB_CLOCK_FREQ);
    }
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}
void USB_DeviceIsrEnable(void)
{
    uint8_t irqNumber;

    uint8_t usbDeviceEhciIrq[] = USBHS_IRQS;
    irqNumber                  = usbDeviceEhciIrq[CONTROLLER_ID - kUSB_ControllerEhci0];

    /* Install isr, set priority, and enable IRQ. */
    NVIC_SetPriority((IRQn_Type)irqNumber, USB_DEVICE_INTERRUPT_PRIORITY);
    EnableIRQ((IRQn_Type)irqNumber);
}
#if USB_DEVICE_CONFIG_USE_TASK
void USB_DeviceTaskFn(void *deviceHandle)
{
    USB_DeviceEhciTaskFunction(deviceHandle);
}
#endif

/*!
 * @brief device msc callback function.
 *
 * This function handle the disk class specified event.
 * @param handle          The USB class  handle.
 * @param event           The USB device event type.
 * @param param           The parameter of the class specific event.
 * @return kStatus_USB_Success or error.
 */
usb_status_t USB_DeviceMscCallback(class_handle_t handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_Error;
    usb_device_lba_information_struct_t *lbaInformationStructure;
    usb_device_lba_app_struct_t *lbaData;
    usb_device_ufi_app_struct_t *ufi;
#if defined(USB_MSC_READ_RESPONSE) && USB_MSC_READ_RESPONSE
    usb_device_capacity_information_struct_t *capacityInformation;
#endif

    switch (event)
    {
        case kUSB_DeviceMscEventReadResponse:
            lbaData = (usb_device_lba_app_struct_t *)param;
            error   = MSC_VFS_ReadResponse(lbaData->offset, lbaData->size, lbaData->buffer);
            break;
        case kUSB_DeviceMscEventWriteResponse:
            lbaData = (usb_device_lba_app_struct_t *)param;

            /* Write MSC data */
            // status_t MSC_VFS_WriteResponse(uint32_t offset, uint32_t size, uint8_t *buffer)
            error = MSC_VFS_WriteResponse(lbaData->offset, lbaData->size, lbaData->buffer);
            break;
        case kUSB_DeviceMscEventWriteRequest:
            lbaData = (usb_device_lba_app_struct_t *)param;

#if 0
        configPRINTF(("[Write Request] lbaData->offset: %d\r\n", lbaData->offset));
        configPRINTF(("[Write Request] lbaData->size: %d\r\n", lbaData->size));
#endif

            /*offset is the write start address get from write command, refer to class driver*/
            lbaData->buffer = g_msc.storageDisk + (lbaData->offset % 0x20) * LENGTH_OF_EACH_LBA;
            break;
        case kUSB_DeviceMscEventReadRequest:
            lbaData = (usb_device_lba_app_struct_t *)param;
            /*offset is the read start address get from read command, refer to class driver*/
            lbaData->buffer = g_msc.storageDisk + (lbaData->offset % 0x20) * LENGTH_OF_EACH_LBA;
            break;
        case kUSB_DeviceMscEventGetLbaInformation:

            lbaInformationStructure                             = (usb_device_lba_information_struct_t *)param;
            lbaInformationStructure->logicalUnitNumberSupported = LOGICAL_UNIT_SUPPORTED;

            lbaInformationStructure->logicalUnitInformations[0].lengthOfEachLba = LENGTH_OF_EACH_LBA;
            lbaInformationStructure->logicalUnitInformations[0].totalLbaNumberSupports =
                TOTAL_LOGICAL_ADDRESS_BLOCKS_NORMAL;
            lbaInformationStructure->logicalUnitInformations[0].bulkInBufferSize  = DISK_LOGICAL_SIZE_NORMAL;
            lbaInformationStructure->logicalUnitInformations[0].bulkOutBufferSize = DISK_LOGICAL_SIZE_NORMAL;

            break;
        case kUSB_DeviceMscEventTestUnitReady:
            /*change the test unit ready command's sense data if need, be careful to modify*/
            ufi = (usb_device_ufi_app_struct_t *)param;
            break;
        case kUSB_DeviceMscEventInquiry:
            ufi         = (usb_device_ufi_app_struct_t *)param;
            ufi->size   = sizeof(usb_device_inquiry_data_fromat_struct_t);
            ufi->buffer = (uint8_t *)&g_InquiryInfo;
            break;
        case kUSB_DeviceMscEventModeSense:
            ufi         = (usb_device_ufi_app_struct_t *)param;
            ufi->size   = sizeof(usb_device_mode_parameters_header_struct_t);
            ufi->buffer = (uint8_t *)&g_ModeParametersHeader;
            break;
        case kUSB_DeviceMscEventModeSelect:
            break;
        case kUSB_DeviceMscEventModeSelectResponse:
            ufi = (usb_device_ufi_app_struct_t *)param;
            break;
        case kUSB_DeviceMscEventFormatComplete:
            break;
        case kUSB_DeviceMscEventRemovalRequest:
            break;
#if defined(USB_MSC_READ_RESPONSE) && USB_MSC_READ_RESPONSE
        case kUSB_DeviceMscEventRequestSense:
            break;
        case kUSB_DeviceMscEventReadCapacity:
            capacityInformation                         = (usb_device_capacity_information_struct_t *)param;
            capacityInformation->lengthOfEachLba        = LENGTH_OF_EACH_LBA;
            capacityInformation->totalLbaNumberSupports = TOTAL_LOGICAL_ADDRESS_BLOCKS_NORMAL;
            break;
        case kUSB_DeviceMscEventReadFormatCapacity:
            capacityInformation                         = (usb_device_capacity_information_struct_t *)param;
            capacityInformation->lengthOfEachLba        = LENGTH_OF_EACH_LBA;
            capacityInformation->totalLbaNumberSupports = TOTAL_LOGICAL_ADDRESS_BLOCKS_NORMAL;
            break;
#endif
        default:
            break;
    }
    return error;
}
/*!
 * @brief device callback function.
 *
 * This function handle the usb standard event. more information, please refer to usb spec chapter 9.
 * @param handle          The USB device handle.
 * @param event           The USB device event type.
 * @param param           The parameter of the device specific request.
 * @return kStatus_USB_Success or error.
 */
usb_status_t USB_DeviceCallback(usb_device_handle handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_Error;
    uint16_t *temp16   = (uint16_t *)param;
    uint8_t *temp8     = (uint8_t *)param;
    switch (event)
    {
        case kUSB_DeviceEventBusReset:
        {
            g_msc.attach               = 0;
            g_msc.currentConfiguration = 0U;
            error                      = kStatus_USB_Success;
#if (defined(USB_DEVICE_CONFIG_EHCI) && (USB_DEVICE_CONFIG_EHCI > 0U)) || \
    (defined(USB_DEVICE_CONFIG_LPCIP3511HS) && (USB_DEVICE_CONFIG_LPCIP3511HS > 0U))
            /* Get USB speed to configure the device, including max packet size and interval of the endpoints. */
            if (kStatus_USB_Success == USB_DeviceClassGetSpeed(CONTROLLER_ID, &g_msc.speed))
            {
                USB_DeviceSetSpeed(handle, g_msc.speed);
            }
#endif
        }
        break;
        case kUSB_DeviceEventSetConfiguration:
            if (0U == (*temp8))
            {
                g_msc.attach               = 0;
                g_msc.currentConfiguration = 0U;
            }
            else if (USB_MSC_CONFIGURE_INDEX == (*temp8))
            {
                g_msc.attach               = 1;
                g_msc.currentConfiguration = *temp8;
            }
            else
            {
                error = kStatus_USB_InvalidRequest;
            }
            break;
        case kUSB_DeviceEventSetInterface:
            if (g_msc.attach)
            {
                uint8_t interface        = (uint8_t)((*temp16 & 0xFF00U) >> 0x08U);
                uint8_t alternateSetting = (uint8_t)(*temp16 & 0x00FFU);
                if (interface < USB_MSC_INTERFACE_COUNT)
                {
                    g_msc.currentInterfaceAlternateSetting[interface] = alternateSetting;
                }
            }
            break;
        case kUSB_DeviceEventGetConfiguration:
            if (param)
            {
                *temp8 = g_msc.currentConfiguration;
                error  = kStatus_USB_Success;
            }
            break;
        case kUSB_DeviceEventGetInterface:
            if (param)
            {
                uint8_t interface = (uint8_t)((*temp16 & 0xFF00U) >> 0x08U);
                if (interface < USB_INTERFACE_COUNT)
                {
                    *temp16 = (*temp16 & 0xFF00U) | g_msc.currentInterfaceAlternateSetting[interface];
                    error   = kStatus_USB_Success;
                }
                else
                {
                    error = kStatus_USB_InvalidRequest;
                }
            }
            break;
        case kUSB_DeviceEventGetDeviceDescriptor:
            if (param)
            {
                error = USB_DeviceGetDeviceDescriptor(handle, (usb_device_get_device_descriptor_struct_t *)param);
            }
            break;
        case kUSB_DeviceEventGetConfigurationDescriptor:
            if (param)
            {
                error = USB_DeviceGetConfigurationDescriptor(handle,
                                                             (usb_device_get_configuration_descriptor_struct_t *)param);
            }
            break;
#if (defined(USB_DEVICE_CONFIG_CV_TEST) && (USB_DEVICE_CONFIG_CV_TEST > 0U))
        case kUSB_DeviceEventGetDeviceQualifierDescriptor:
            if (param)
            {
                /* Get Qualifier descriptor request */
                error = USB_DeviceGetDeviceQualifierDescriptor(
                    handle, (usb_device_get_device_qualifier_descriptor_struct_t *)param);
            }
            break;
#endif
        case kUSB_DeviceEventGetStringDescriptor:
            if (param)
            {
                error = USB_DeviceGetStringDescriptor(handle, (usb_device_get_string_descriptor_struct_t *)param);
            }
            break;
        default:
            break;
    }
    return error;
}
/* USB device class information */
usb_device_class_config_struct_t msc_config[1] = {{
    USB_DeviceMscCallback,
    0,
    &g_UsbDeviceMscConfig,
}};
/* USB device class configuration information */
usb_device_class_config_list_struct_t msc_config_list = {
    msc_config,
    USB_DeviceCallback,
    1,
};

/*!
 * @brief device application init function.
 *
 * This function init the usb stack and sdhc driver.
 *
 * @return None.
 */
void USB_DeviceApplicationInit()
{
    USB_DeviceClockInit();
#if (defined(FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT > 0U))
    SYSMPU_Enable(SYSMPU, 0);
#endif /* FSL_FEATURE_SOC_SYSMPU_COUNT */

    g_msc.speed        = USB_SPEED_FULL;
    g_msc.attach       = 0;
    g_msc.mscHandle    = (class_handle_t)NULL;
    g_msc.deviceHandle = NULL;

    MSC_VFS_Init(&s_StorageDisk[0], &g_msc.application_task_handle, LENGTH_OF_EACH_LBA);

    g_msc.storageDisk = &s_StorageDisk[0];

    if (kStatus_USB_Success != USB_DeviceClassInit(CONTROLLER_ID, &msc_config_list, &g_msc.deviceHandle))
    {
        configPRINTF(("USB device init failed\r\n"));
    }
    else
    {
        configPRINTF(("\r\n\r\n"));
        configPRINTF(("USB device mass storage enabled\r\n\r\n"));
        configPRINTF(("File System update:               waiting for %s.bin file of maximum size %d KB\r\n", FILENAME_FS, (FICA_FILE_SYS_SIZE / 1024)));
        configPRINTF(("Bank A Application update:        waiting for %s.bin file of maximum size %d KB\r\n", FILENAME_APP_A, (FICA_IMG_APP_A_SIZE / 1024)));
        configPRINTF(("Bank B Application update:        waiting for %s.bin file of maximum size %d KB\r\n", FILENAME_APP_B, (FICA_IMG_APP_A_SIZE / 1024)));
        configPRINTF(("Opposite Bank Application update: waiting for *.bin file of maximum size %d KB\r\n", (FICA_IMG_APP_A_SIZE / 1024)));
        configPRINTF(("\r\n\r\n"));

        g_msc.mscHandle = msc_config_list.config->classHandle;
    }

    USB_DeviceIsrEnable();

#if defined(USB_MSC_FORCE_DELAY) && USB_MSC_FORCE_DELAY
    /*Add one delay here to make the DP pull down long enough to allow host to detect the previous disconnection.*/
    SDK_DelayAtLeastUs(5000, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
#endif

    USB_DeviceRun(g_msc.deviceHandle);
}

int32_t USB_Transfer_Monitor(void)
{
    int32_t status                = SLN_FLASH_NO_ERROR;
    static uint32_t blinkcount    = 0;
    static bool blinktoggle       = true;
    msc_vfs_state_t transferState = MSC_VFS_GetTransferState();

    switch (transferState)
    {
        case TRANSFER_IDLE:
            RGB_LED_Blink(LED_BRIGHT_LOW, LED_COLOR_PURPLE, BLINK_RATE, &blinkcount, &blinktoggle);
            break;
        case TRANSFER_START:
        case TRANSFER_ACTIVE:
            RGB_LED_Blink(LED_BRIGHT_LOW, LED_COLOR_BLUE, BLINK_RATE, &blinkcount, &blinktoggle);
            break;
        case TRANSFER_FINAL:
            RGB_LED_SetBrightnessColor(LED_BRIGHT_LOW, LED_COLOR_GREEN);
            break;
        case TRANSFER_ERROR:
        default:
            break;
    };

    return status;
}

void USB_Transfer_Finalize(void)
{
    int32_t status                = SLN_FLASH_NO_ERROR;
    msc_vfs_state_t transferState = MSC_VFS_GetTransferState();
    fica_img_type_t transferType  = MSC_VFS_GetTransferType();
    bool newFilePending           = false;

    if (TRANSFER_FINAL == transferState)
    {
        if ((transferType == FICA_IMG_TYPE_FS))
        {
            configPRINTF(("File System updated!\r\n\r\n"));
        }
        else
        {
            // Finalize the new application by storing app specific info into FICA
            status = FICA_app_program_ext_finalize(NULL);

            // Make the new app active by setting the reset vector
            if (SLN_FLASH_NO_ERROR == status)
            {
                status = FICA_app_program_ext_set_reset_vector();
            }

            if (SLN_FLASH_NO_ERROR == status)
            {
                configPRINTF(("Application updated!\r\n\r\n"));
            }
            else
            {
                MSC_VFS_SetTransferState(TRANSFER_ERROR);
                configPRINTF(("[Error] Application update failed!\r\n"));
            }
        }

        MSC_VFS_SetFileProcessed();
        newFilePending = MSC_VFS_WaitNewFile(2000);
    }
    else
    {
        configPRINTF(("[Error] MSD process error\r\n"));
    }

    if (newFilePending == false)
    {
        /* If no new pending files, finish MSD */
        configPRINTF(("MSD process finished\r\n\r\n"));

        if (TRANSFER_ERROR == transferState)
        {
            ErrorBlinkLED();
        }

        vTaskDelay(100);

        // Stop the USB before we do anything further
        USB_DeviceStop(g_msc.deviceHandle);

        vTaskDelay(100);

#if ENABLE_ENCRYPTION
        // Must reboot when running encrypted XIP
        if (bl_nor_encrypt_is_enabled())
        {
            reset_mcu();
        }
        else
        {
#endif /* ENABLE_ENCRYPTION */
            ReRunBootloader();
#if ENABLE_ENCRYPTION
        }
#endif /* ENABLE_ENCRYPTION */
    }
}

void USB_DeviceTask(void *handle)
{
    while (1)
    {
        // Monitor the transfer state
        USB_Transfer_Monitor();

#if USB_DEVICE_CONFIG_USE_TASK
        USB_DeviceTaskFn(g_msc.deviceHandle);
#endif
    }
}

void USB_MSC_TASK(void *handle)
{
    USB_DeviceApplicationInit();

#if USB_DEVICE_CONFIG_USE_TASK
    if (g_msc.deviceHandle)
    {
        if (xTaskCreate(USB_DeviceTask,                  /* pointer to the task */
                        (char const *)"usb device task", /* task name for kernel awareness debugging */
                        5000L / sizeof(portSTACK_TYPE),  /* task stack size */
                        g_msc.deviceHandle,              /* optional task startup argument */
                        5,                               /* initial priority */
                        &g_msc.device_task_handle        /* optional task handle to create */
                        ) != pdPASS)
        {
            configPRINTF(("usb msc task create failed!\r\n"));
            return;
        }
    }
#endif

    while (1)
    {
        // Suspend this task until we are ready to finalize the data transfer
        vTaskSuspend(NULL);

        // Finalize transfer
        USB_Transfer_Finalize();
    }
}

int USB_MSC_Init(void)
{
    if (xTaskCreate(USB_MSC_TASK,                   /* pointer to the task */
                    "USB MSC TASK",                 /* task name for kernel awareness debugging */
                    5000L / sizeof(portSTACK_TYPE), /* task stack size */
                    &g_msc,                         /* optional task startup argument */
                    4,                              /* initial priority */
                    &g_msc.application_task_handle  /* optional task handle to create */
                    ) != pdPASS)
    {
        configPRINTF(("usb msc task create failed!\r\n"));
        return 1;
    }

    return 1;
}

bool CheckForMSDMode(void)
{
    bool update = false;

    /* Check if USB MSD Mode button is pushed */
    if (PUSH_BUTTONS_MSDPressed())
    {
        PerformMSD();
        update = true;
    }

    return update;
}

void PerformMSD(void)
{
    /* Check if USB MSD Mode button is pushed */
#if ENABLE_UNSIGNED_USB_MSD
    FICA_set_usb_mode();
#endif /* ENABLE_UNSIGNED_USB_MSD */

    USB_MSC_Init();
}

void SLN_MSD_FallbackSet(bool state)
{
    s_fallback = state;
}

bool SLN_MSD_FallbackGet(void)
{
    return s_fallback;
}
