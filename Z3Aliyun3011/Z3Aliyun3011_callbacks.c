// This callback file is created for your convenience. You may add application
// code to this file. If you regenerate this file over a previous version, the
// previous version will be overwritten and any code you have added will be
// lost.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app/framework/include/af.h"
#include "app/framework/plugin/network-creator/network-creator.h"
#include "device-table/device-table.h"
#include "../../../platform/emdrv/nvm3/inc/nvm3.h"
#include "../../../platform/emdrv/nvm3/inc/nvm3_default.h"

#include "wifi/wgm110.h"
#include "iotkit-embedded-sdk/aliyun_main.h"
#include "iotkit-embedded-sdk/wrappers/wrappers_defs.h"

extern int HAL_Timer_Task_Init();
extern uint8_t *emAfZclBuffer;
extern uint16_t emAfZclBufferLen;
extern uint16_t *emAfResponseLengthPtr;
extern EmberApsFrame *emAfCommandApsFrame;

#define POLL_ATTR_INTERVAL 6000
#define FRESH_DEV_INTERVAL 3000

static uint8_t    g_no_connect_cnt = 0;
static uint8_t    g_app_task_can_run = 0;
static uint8_t    g_button1_pressed = 0;
static uint16_t   g_poll_interval = POLL_ATTR_INTERVAL;

EmberEventControl pollAttrEventControl;
EmberEventControl clearWiFiEventControl;
EmberEventControl addSubDevEventControl;
EmberEventControl commissionEventControl;

typedef struct
{
    uint16_t    clusterID;
    uint16_t    attrID;
}PollItem_S;

static const PollItem_S g_polllist[] = 
{
    {ZCL_ON_OFF_CLUSTER_ID,             ZCL_ON_OFF_ATTRIBUTE_ID},
    {ZCL_LEVEL_CONTROL_CLUSTER_ID,      ZCL_CURRENT_LEVEL_ATTRIBUTE_ID},
//    {ZCL_COLOR_CONTROL_CLUSTER_ID,      ZCL_COLOR_CONTROL_COLOR_TEMPERATURE_ATTRIBUTE_ID},
};
static uint8_t  g_current_poll_index = 0;

void emberAfPollAttrByDeviceTable()
{
    EmberStatus status;
    uint8_t attributeIdBuffer[2];
    EmberAfPluginDeviceTableEntry *deviceTable = emberAfDeviceTablePointer();
    uint8_t  i;
    
    #if  1
    uint8_t  offlinecnt = 0;

    for (i = 0; i < EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE; i++) {
        if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID == deviceTable[i].nodeId ||
            EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED != deviceTable[i].state ||
            (emberGetNodeId() == deviceTable[i].nodeId) ||
            (DEMO_Z3DIMMERLIGHT != deviceTable[i].deviceId &&
             DEMO_Z3CURTAIN != deviceTable[i].deviceId)) {
            continue;
        }

        if (1 != deviceTable[i].online) {
            offlinecnt++;
        }
    }

    if (offlinecnt > 0) {
        g_poll_interval = POLL_ATTR_INTERVAL;
    } else {
        g_poll_interval = 3*POLL_ATTR_INTERVAL;
    }
    #endif /* #if 0 */

    #if  0
    if (true != aliyun_is_cloud_connected()) {
        g_poll_interval = POLL_ATTR_INTERVAL;
    } else {
        g_poll_interval = 3*POLL_ATTR_INTERVAL;
    }
    #endif /* #if 0 */
    
    attributeIdBuffer[0] = LOW_BYTE(g_polllist[g_current_poll_index].attrID);
    attributeIdBuffer[1] = HIGH_BYTE(g_polllist[g_current_poll_index].attrID);
    emberAfFillCommandGlobalClientToServerReadAttributes(g_polllist[g_current_poll_index].clusterID, attributeIdBuffer, sizeof(attributeIdBuffer));

    emAfCommandApsFrame->profileId = HA_PROFILE_ID;
    emAfCommandApsFrame->sourceEndpoint = 1;
    emAfCommandApsFrame->destinationEndpoint = 255;


    EmberMessageBuffer payload = emberFillLinkedBuffers(emAfZclBuffer, *emAfResponseLengthPtr);
    if (payload == EMBER_NULL_MESSAGE_BUFFER) {
        emberAfCorePrintln("[%d] no enough buffer", __LINE__);
        return;
    }
    
    status = emberSendBroadcast(EMBER_RX_ON_WHEN_IDLE_BROADCAST_ADDRESS,
                                emAfCommandApsFrame,
                                1,
                                payload);
    emberReleaseMessageBuffer(payload);
    
    emberAfCorePrintln("[%d] broadcast read cluster[%X] attr[%X] status=%X", __LINE__, 
                                                                             g_polllist[g_current_poll_index].clusterID,
                                                                             g_polllist[g_current_poll_index].attrID,
                                                                             status);
    if (EMBER_SUCCESS != status) {
        g_poll_interval = 3*POLL_ATTR_INTERVAL;
        return;
    }

    g_current_poll_index = (g_current_poll_index + 1) % (sizeof(g_polllist) / sizeof(PollItem_S));

    for (i = 0; i < EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE; i++) {
        if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID == deviceTable[i].nodeId ||
            EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED != deviceTable[i].state ||
            (DEMO_Z3DIMMERLIGHT != deviceTable[i].deviceId &&
             DEMO_Z3CURTAIN != deviceTable[i].deviceId)) {
            continue;
        }

        deviceTable[i].keepalive_failcnt++;
        if (deviceTable[i].keepalive_failcnt > 6) {
            
            if (0 != deviceTable[i].online) {
                emberEventControlSetActive(addSubDevEventControl);    
            }
            
            deviceTable[i].online = 0;
            deviceTable[i].keepalive_failcnt = 0;
        }
    }
}

void pollAttrEventHandler()
{
    emberEventControlSetInactive(pollAttrEventControl);

    if (emberAfNetworkState() != EMBER_JOINED_NETWORK) {
        return;
    }

    if (true != aliyun_is_cloud_connected()) {
        emberEventControlSetDelayMS(pollAttrEventControl, g_poll_interval);
        g_no_connect_cnt++;
        if (g_no_connect_cnt > 6) {
            halReboot();
        }
        return;
    }

    emberAfPollAttrByDeviceTable();
    emberEventControlSetDelayMS(pollAttrEventControl, g_poll_interval);
}

void clearWiFiEventHandler()
{
    int ret;
    
    emberEventControlSetInactive(clearWiFiEventControl);
    
    ret = wifi_erase_alldata();
    emberAfCorePrintln("clear wifi ret=%d", ret);
    if (0 == ret) {
        halReboot();
    }
}

void addSubDevEventHandler()
{
    uint8_t     cloud_oper_cnt = 0;
    EmberEUI64  nulleui64 = {0xFF};
    
    emberEventControlSetInactive(addSubDevEventControl);
    //emberAfCorePrintln("[%s][%d]trace", __func__, __LINE__);
    
    if (true != aliyun_is_cloud_connected()) {
        emberEventControlSetDelayMS(addSubDevEventControl, FRESH_DEV_INTERVAL);
        return;
    }

    EmberAfPluginDeviceTableEntry *deviceTable = emberAfDeviceTablePointer();
    uint8_t i;

    //emberAfCorePrintln("[%s][%d]trace", __func__, __LINE__);

    memset(nulleui64, 0xFF, sizeof(nulleui64));
    for (i = 0; i < EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE; i++) {
        if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID != deviceTable[i].nodeId &&
            EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED == deviceTable[i].state &&
            (emberGetNodeId() != deviceTable[i].nodeId) &&
            0 != memcmp(nulleui64, deviceTable[i].eui64, sizeof(nulleui64)) &&
            (DEMO_Z3DIMMERLIGHT == deviceTable[i].deviceId ||
             DEMO_Z3CURTAIN == deviceTable[i].deviceId)) {

            //emberAfCorePrintln("[%s][%d]cloud_devid=%d online=%d", __func__, __LINE__, deviceTable[i].cloud_devid, deviceTable[i].online);
            if (deviceTable[i].cloud_devid <= 0 &&
                deviceTable[i].online == 1) {
                aliyun_add_subdev(deviceTable[i].eui64, 
                                  deviceTable[i].endpoint, 
                                  deviceTable[i].deviceId, 
                                  &deviceTable[i].cloud_devid);
                cloud_oper_cnt++;
            } else if (deviceTable[i].cloud_devid > 0 && 
                       deviceTable[i].online != 1) {
                emberAfCorePrintln("[%d] node %X ep %d cloudid=%d", i, 
                                                                    deviceTable[i].nodeId,
                                                                    deviceTable[i].endpoint,
                                                                    deviceTable[i].cloud_devid);
                aliyun_del_subdev(deviceTable[i].cloud_devid);
                deviceTable[i].cloud_devid = -1;
                cloud_oper_cnt++;
            }
        }
    }

    emberEventControlSetDelayMS(addSubDevEventControl, FRESH_DEV_INTERVAL);
}

void commissionEventHandler()
{
    EmberStatus status;

    emberEventControlSetInactive(commissionEventControl);
    if (emberAfNetworkState() == EMBER_JOINED_NETWORK) {
        if (1 == g_button1_pressed) {
            nvm3_eraseAll(nvm3_defaultHandle);
            halReboot();
        }
    } else {
        status = emberAfPluginNetworkSteeringStart();
        emberAfCorePrintln("start to steering, status=%X...", status);
    }
}

void emberAfHalButtonIsrCallback(int8u button, int8u state)
{
    if (BUTTON0 == button) {
        if (state == BUTTON_PRESSED) {
            emberEventControlSetDelayMS(clearWiFiEventControl, 10000);
        } else {
            emberEventControlSetInactive(clearWiFiEventControl);
        }
    }

    if (BUTTON1 == button) {
        if (state == BUTTON_PRESSED) {
            emberEventControlSetDelayMS(commissionEventControl, 10000);
            g_button1_pressed = 1;
        } else {
            g_button1_pressed = 0;
            emberEventControlSetInactive(commissionEventControl);
        }
    }
}

void emberAfCloudConnectedHandler()
{
    /*use led1 to indicate if the cloud is connected.  */
    halSetLed(BOARDLED0);
}

/** @brief Main Init
 *
 * This function is called from the application's main function. It gives the
 * application a chance to do any initialization required at system startup. Any
 * code that you would normally put into the top of the application's main()
 * routine should be put into this function. This is called before the clusters,
 * plugins, and the network are initialized so some functionality is not yet
 * available.
        Note: No callback in the Application Framework is
 * associated with resource cleanup. If you are implementing your application on
 * a Unix host where resource cleanup is a consideration, we expect that you
 * will use the standard Posix system calls, including the use of atexit() and
 * handlers for signals such as SIGTERM, SIGINT, SIGCHLD, SIGPIPE and so on. If
 * you use the signal() function to register your signal handler, please mind
 * the returned value which may be an Application Framework function. If the
 * return value is non-null, please make sure that you call the returned
 * function from your handler to avoid negating the resource cleanup of the
 * Application Framework itself.
 *
 */
void emberAfMainInitCallback(void)
{
    emberEventControlSetActive(commissionEventControl);
    emberEventControlSetDelayMS(addSubDevEventControl, FRESH_DEV_INTERVAL);
}

boolean emberAfStackStatusCallback(EmberStatus status)
{
    if (status == EMBER_NETWORK_DOWN) {
        halClearLed(BOARDLED1);
    } else if (status == EMBER_NETWORK_UP) {
        halSetLed(BOARDLED1);
        emberEventControlSetDelayMS(pollAttrEventControl, g_poll_interval);
        g_app_task_can_run = 1;
    }
}

/** @brief
 *
 * This function is called from the Micrium RTOS plugin before the
 * Application (1) task is created.
 */
void emberAfPluginMicriumRtosAppTask1InitCallback(void)
{
	if (0 != HAL_Timer_Task_Init()) {
		printf("[%s][%d]init timer failed", __func__, __LINE__);
        halReboot();
		return;
	}
}

/** @brief
 *
 * This function implements the Application (1) task main loop.
 *
 * @param p_arg Ver.: always
 */
void emberAfPluginMicriumRtosAppTask1MainLoopCallback(void *p_arg)
{
    bool wifi_connected = false;
    int  retry = 0;

    while (!g_app_task_can_run) {
        HAL_SleepMs(10);
    }
    
	if (0 != wifi_init()) {
		printf("[%s][%d]init wifi failed", __func__, __LINE__);
        halReboot();
		return;
	}

    if (wifi_is_ssid_valid())
    {
        while (1/*retry++ < 3*/) {
            if (0 == wifi_connect_withsaveddata(3000)) {
                wifi_connected = true;
                break;
            }
        }

        #if  0
        if (retry >= 3) {
            wifi_connected = false;
        }
        #endif /* #if 0 */
    } else {
        wifi_connected = false;
    }

	/*reset if failed to connect to cloud  */
    if (0 != aliyun_main(wifi_connected)) {
        halReboot();
    }
}

void emberAfPluginDeviceTableNewDeviceCallback(EmberEUI64 eui64)
{
    //emberEventControlSetDelayMS(addSubDevEventControl, 5000);
    
    EmberAfPluginDeviceTableEntry *deviceTable = emberAfDeviceTablePointer();
    uint8_t i;

    
    for (i = 0; i < EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE; i++) {
        if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID != deviceTable[i].nodeId &&
            EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED == deviceTable[i].state) {
            if (0 == MEMCOMPARE(eui64, deviceTable[i].eui64, EUI64_SIZE)) {
                deviceTable[i].online = 1;
                deviceTable[i].keepalive_failcnt = 0;
                deviceTable[i].keepalive_seq = 0;
                deviceTable[i].cloud_devid = -1;
            }
        }
    }
}

void emberAfPluginDeviceTableDeviceLeftCallback(EmberEUI64 eui64)
{
    //emberEventControlSetDelayMS(addSubDevEventControl, 5000);
    
    EmberAfPluginDeviceTableEntry *deviceTable = emberAfDeviceTablePointer();
    uint8_t i;

    for (i = 0; i < EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE; i++) {
        if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID != deviceTable[i].nodeId &&
            EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED == deviceTable[i].state) {
            if (0 == MEMCOMPARE(eui64, deviceTable[i].eui64, EUI64_SIZE)) {
                deviceTable[i].online = 0;
                deviceTable[i].keepalive_failcnt = 0xFF;
                deviceTable[i].keepalive_seq = 0;
            }
        }
    }
}

void emAfIEEEDiscoveryCallback(const EmberAfServiceDiscoveryResult* result)
{
    if (!emberAfHaveDiscoveryResponseStatus(result->status)) {
        return;
    }

    uint8_t* eui64ptr = (uint8_t*)(result->responseData);
    emberAfDeviceTableNewDeviceJoinHandler(result->matchAddress, eui64ptr);
}

boolean emberAfReadAttributesResponseCallback(EmberAfClusterId clusterId, int8u *buffer, int16u bufLen)
{
    EmberNodeId nodeID;
    uint16_t    index;
    char        properties[256] = {0};
    EmberAfPluginDeviceTableEntry *deviceTable = emberAfDeviceTablePointer();

    nodeID = emberGetSender();
    if (nodeID == emberGetNodeId()) {
        return false;
    }
    
    index = emberAfDeviceTableGetEndpointFromNodeIdAndEndpoint(nodeID, emberAfCurrentCommand()->apsFrame->sourceEndpoint);
    if (EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_INDEX == index) {
        emberAfFindIeeeAddress(nodeID, emAfIEEEDiscoveryCallback);
        return false;
    } /*else if (EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED != deviceTable[index].state) {
        emberAfFindIeeeAddress(nodeID, emAfIEEEDiscoveryCallback);
        return false;
    }*/

    if (1 != deviceTable[index].online) {
        emberEventControlSetActive(addSubDevEventControl);    
    }
    
    deviceTable[index].online = 1;
    deviceTable[index].keepalive_failcnt = 0;

    #if  0
    if (ZCL_ON_OFF_CLUSTER_ID == clusterId) {
        EmberAfAttributeId attributeId = (EmberAfAttributeId)emberAfGetInt16u(buffer, 0, bufLen);
        EmberAfStatus status = (EmberAfStatus)emberAfGetInt8u(buffer, 2, bufLen);
        uint8_t val = (uint8_t)emberAfGetInt8u(buffer, 4, bufLen);

        //emberAfCorePrintln("[%s][%d]on-off resp status=%X val=%d", __func__, __LINE__, status, val);

        if (EMBER_ZCL_STATUS_SUCCESS == status && ZCL_ON_OFF_ATTRIBUTE_ID == attributeId) {
            switch (deviceTable[index].deviceId)
            {
                case DEMO_Z3DIMMERLIGHT:
                    snprintf(properties, sizeof(properties), "{\"LightSwitch\":%d}", val);
                    break;
                default:
                    break;
            }
        }
    }

    if (ZCL_LEVEL_CONTROL_CLUSTER_ID == clusterId) {
        EmberAfAttributeId attributeId = (EmberAfAttributeId)emberAfGetInt16u(buffer, 0, bufLen);
        EmberAfStatus status = (EmberAfStatus)emberAfGetInt8u(buffer, 2, bufLen);
        uint8_t val = (uint8_t)emberAfGetInt8u(buffer, 4, bufLen);

        //emberAfCorePrintln("[%s][%d]level-control resp status=%X val=%d", __func__, __LINE__, status, val);

        if (EMBER_ZCL_STATUS_SUCCESS == status && ZCL_CURRENT_LEVEL_ATTRIBUTE_ID == attributeId) {
            switch (deviceTable[index].deviceId)
            {
                case DEMO_Z3DIMMERLIGHT:
                    snprintf(properties, sizeof(properties), "{\"Brightness\":%d}", (uint32_t)val * 100 / 255);
                    break;
                case DEMO_Z3CURTAIN:
                    snprintf(properties, sizeof(properties), "{\"CurtainPosition\":%d}", (uint32_t)val * 100 / 255);
                    break;
                default:
                    break;
            }            
        }
    }

    if (deviceTable[index].cloud_devid > 0 && strlen(properties) > 0) {
        aliyun_post_property(deviceTable[index].cloud_devid, properties);
    }
    #endif /* #if 0 */

    return false;
}

static void kv_show(void)
{
	uint8_t i;
	tokTypeKvs data;

	printf("read:\r\n");
	for (i = 0; i < MAX_KV_NUMBER; i++) {
		memset(&data, 0, sizeof(data));
		halCommonGetIndexedToken(&data, TOKEN_KV_PAIRS, i);
		if ('\0' != data.kv_key[0]) {
			printf("i=%d key=%s len=%d \r\n", i, data.kv_key, data.value_len);
		}
	}
}

static void kv_add(void)
{
	int       ret;
    uint8_t   vallen;
    uint8_t  *value = NULL;
    char      key[64] = {0};
    char      val[64] = {0};

    //DYNAMIC_REG_
    value = emberStringCommandArgument(0, &vallen);
    if (vallen >= sizeof(key)) {
        printf("input key too long\r\n");
        return;
    }
    MEMCOPY(key, value, vallen);
    
    value = emberStringCommandArgument(1, &vallen);
    if (vallen >= sizeof(key)) {
        printf("input value too long\r\n");
        return;
    }
    MEMCOPY(val, value, vallen);

	ret = HAL_Kv_Set(key, val, strlen(val) + 1, 0);

	printf("add ret:%d\r\n", ret);
}

static void dev_show(void)
{
	uint8_t i;
	tokTypeDevTable data;

	printf("read:\r\n");
	for (i = 0; i < MAX_DEV_TABLE_NUMBER; i++) {
		memset(&data, 0xFF, sizeof(data));
		halCommonGetIndexedToken(&data, TOKEN_DEV_TABLE, i);
		if (0xFFFF != data.nodeId) {
			printf("i=%d nodeId=0x%04X ep=%d \r\n", i, data.nodeId, data.endpoint);
		}
	}
}

static void wifi_clear(void)
{
    int ret;

    ret = wifi_erase_alldata();
    printf("erase all wifi data ret=%d\r\n", ret);
}

static void em_showtxpwr(void)
{
    tokTypeStackNodeData data;

    memset(&data, 0xFF, sizeof(data));
    halCommonGetToken(&data, TOKEN_STACK_NODE_DATA);
    printf("txpwr:%d\r\n", data.radioTxPower);
}

static void em_settxpower(void)
{
    tokTypeStackNodeData data;

    uint8_t pwr = (uint8_t)emberUnsignedCommandArgument(0);

    memset(&data, 0xFF, sizeof(data));
    halCommonGetToken(&data, TOKEN_STACK_NODE_DATA);
    data.radioTxPower = pwr;
    halCommonSetToken(TOKEN_STACK_NODE_DATA, &data);
}

EmberCommandEntry emberAfCustomCommands[] = {
  emberCommandEntryAction("kvshow", kv_show, "", ""),
  emberCommandEntryAction("kvadd",  kv_add, "bb", ""),
  emberCommandEntryAction("devshow", dev_show, "", ""),
  emberCommandEntryAction("wificlear", wifi_clear, "", ""),
  emberCommandEntryAction("showtxpwr", em_showtxpwr, "", ""),
  emberCommandEntryAction("settxpwr", em_settxpower, "u", ""),
  emberCommandEntryTerminator()
};
