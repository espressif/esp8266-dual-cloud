#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "jd_innet.h"
#include "joylink_smnt.h"
#include "joylink_smnt_adp.h"
#include "joylink_log.h"

struct RxControl {
    signed rssi: 8;
    unsigned rate: 4;
    unsigned is_group: 1;
    unsigned: 1;
    unsigned sig_mode: 2;
    unsigned legacy_length: 12;
    unsigned damatch0: 1;
    unsigned damatch1: 1;
    unsigned bssidmatch0: 1;
    unsigned bssidmatch1: 1;
    unsigned MCS: 7;
    unsigned CWB: 1;
    unsigned HT_length: 16;
    unsigned Smoothing: 1;
    unsigned Not_Sounding: 1;
    unsigned: 1;
    unsigned Aggregation: 1;
    unsigned STBC: 2;
    unsigned FEC_CODING: 1;
    unsigned SGI: 1;
    unsigned rxend_state: 8;
    unsigned ampdu_cnt: 8;
    unsigned channel: 4;
    unsigned: 12;
};

struct Ampdu_Info
{
    uint16 length;
    uint16 seq;
    uint8  address3[6];
};

struct sniffer_buf {
    struct RxControl rx_ctrl;
    uint8_t  buf[36];
    uint16_t cnt;
    struct Ampdu_Info ampdu_info[1];
};

struct sniffer_buf2{
    struct RxControl rx_ctrl;
    uint8 buf[496];
    uint16 cnt;
    uint16 len; //length of packet
};

typedef struct jd_sniffer{
    uint8_t  current_channel;
    uint16_t channel_bits;
    uint8    stop_flag;
    uint8    scan_flag;
    uint8    link_flag;
	uint8    channel_count;
}JD_SNIFFER, *pJS_SNIFFER;

//#define AES_KEY  "C423GR98JZHEYXYR"
static const char *AES_KEY = NULL;
JD_SNIFFER   JDSnifGlob = {0};
static os_timer_t channel_timer;
static unsigned char* s_pBuffer = NULL;
// static xSemaphoreHandle s_sem_check_result = NULL;
bool aws_get_wifi = false;
/* set channel */
uint8_t adp_changeCh(int i)
{
    wifi_set_channel(i);
    return 1; //must to change channel
}

void jd_innet_check_result(void);


void ICACHE_FLASH_ATTR
joylink_config_rx_callback(uint8_t *buf, uint16_t buf_le)
{
	uint8 one,i;
    uint16 len, cnt_packk = 0;
	PHEADER_802_11_SMNT frame;
    joylinkResult_t Ret;
    struct sniffer_buf *pack_all = (struct sniffer_buf *)buf;
	if(1 == JDSnifGlob.stop_flag){
        return;
    }

    if (buf_le == sizeof(struct sniffer_buf2)) { /* managment frame */
        return;
    }
    
    if (buf_le == 12 || buf_le == 128) {
        return;
    } else {
		while (cnt_packk < (pack_all->cnt)) {
            len = pack_all->ampdu_info[cnt_packk].length;//get all package length
			cnt_packk++;
			/* FRAME_SUBTYPE_UDP_MULTIANDBROAD */
			frame = (PHEADER_802_11_SMNT)pack_all->buf;
            joylink_cfg_DataAction(frame, len);
            if (joylink_cfg_Result(&Ret) == 0) {
                if (Ret.type != 0) {
                    JDSnifGlob.stop_flag = 1;
                    wifi_promiscuous_enable(0);
                    // xSemaphoreGive(s_sem_check_result);
                    jd_innet_check_result();
                    return;
                }
            }
			/* FRAME_SUBTYPE_BEACON */
			/* FRAME_SUBTYPE_RAWDATA */
			/* FRAME_SUBTYPE_UNKNOWN */
		}
	}
}

/* timer to change channel */
void ICACHE_FLASH_ATTR
joylink_config_loop_do(void *arg)
{
	if(1 == JDSnifGlob.stop_flag){
        return;
    }
    //joylink_innet_timingcall();
	//uint8_t ret = joylink_cfg_50msTimer();
}
static char joylink_ssid[32];
static char joylink_password[64];

int joylink_get_ssid_passwd(char ssid[32 + 1], char passwd[64 + 1])
{
    log_info("joylink_get_ssid_passwd:%s, %s", joylink_ssid,joylink_password);
    memcpy(ssid,joylink_ssid,sizeof(joylink_ssid));
    memcpy(passwd,joylink_password,sizeof(joylink_password));
}

bool jd_innet_get_result()
{
    uint8_t ssid_len = 0, pass_len = 0;
    int len = 0;
    bool rsp = FALSE;
    joylinkResult_t Ret;    
    uint8_t AES_IV[16];
    uint8_t jd_aes_out_buffer[128];
    bzero(AES_IV, 16);
    bzero(jd_aes_out_buffer, 128);
    if (0 == joylink_cfg_Result(&Ret)) {
        if (Ret.type != 0) {
            log_info("innet ret type: %d", Ret.type);
            len = device_aes_decrypt((const uint8 *)AES_KEY, strlen(AES_KEY),AES_IV,
                Ret.encData + 1,Ret.encData[0], jd_aes_out_buffer,sizeof(jd_aes_out_buffer));
            if (len > 0) {
                if (jd_aes_out_buffer[0] > 32) {
                    log_error("sta password len error");
                    return rsp;
                }
                pass_len = jd_aes_out_buffer[0];
                if (pass_len > sizeof(joylink_password)) {
                    log_error("sta password len error");
                    return rsp;
                }
                
                memcpy(joylink_password,jd_aes_out_buffer + 1, pass_len);
                ssid_len = len - 1 - pass_len - 4 - 2;
                if (ssid_len > sizeof(joylink_ssid)) {
                    log_error("sta ssid len error");
                    return rsp;
                }
                strncpy(joylink_ssid,(const char*)(jd_aes_out_buffer + 1 + pass_len + 4 + 2), ssid_len);
                if (0 == joylink_ssid[0]) {
                    log_error("sta ssid error");
                    return rsp;
                }
                log_info("ssid len:%d, ssid:%s", ssid_len,joylink_ssid);
                // log_info("pass len:%d, password:%s", pass_len, config.password);
               {
                    log_info("set sta ok\r\n");
                    jd_innet_stop();
                    aws_get_wifi = true;
                    // wifi_station_connect();
                    rsp = TRUE;
                    log_info("aws_get_wifi:%d",aws_get_wifi);
                }
            } else {
                log_error("aes fail\r\n");
                return rsp;
            }
        }
    }else {
        log_error("result fail\r\n");
        return rsp;
    }

    
    return rsp;
}

void jd_innet_check_result(void)
{
    if (TRUE == jd_innet_get_result()){
        log_info("innet retry");
        jd_innet_stop();
    }

}
#if 0
void jd_innet_start_task(void *para)
{
INNET_RETRY:
    JDSnifGlob.stop_flag = 0;
	// wifi_station_disconnect();
	// wifi_set_opmode(STATION_MODE);
    
    while (1){
        if(pdTRUE == xSemaphoreTake(s_sem_check_result, portMAX_DELAY)) {//
            if (FALSE == jd_innet_get_result()){
                log_info("innet retry");
                jd_innet_stop();
                goto INNET_RETRY;
            }
            log_debug("high water %d ",uxTaskGetStackHighWaterMark(NULL));
            break;
        }
    }
    vSemaphoreDelete(s_sem_check_result);
    s_sem_check_result = NULL;
    vTaskDelete(NULL);
}
#endif 
bool jd_innet_start()
{  
    if (NULL == AES_KEY) {
       return FALSE;
    }
    if(NULL == s_pBuffer) {
        s_pBuffer = (unsigned char*)malloc(1024 - 512);
    }
    if (s_pBuffer == NULL) {
        log_error("malloc err");
        return false;
    }
    #if 0
    s_sem_check_result = (xSemaphoreHandle)xSemaphoreCreateBinary();
    if (s_sem_check_result == NULL) {
        log_error("");
        return false;
    }
    #endif
    log_info("innet start");
    joylink_cfg_init(s_pBuffer);
    log_info("*********************************");
    log_info("*     ENTER SMARTCONFIG MODE    *");
    log_info("*********************************");
    JDSnifGlob.stop_flag = 0;
    // xTaskCreate(jd_innet_start_task, "innet st", 1024-128, NULL, tskIDLE_PRIORITY + 2, NULL);
    return TRUE;
}

void ICACHE_FLASH_ATTR 
jd_innet_stop(void)
{
	JDSnifGlob.stop_flag = 1;
	// wifi_promiscuous_enable(0);
	if (s_pBuffer) {
        free(s_pBuffer);
        s_pBuffer = NULL;
    }
}

bool jd_innet_set_aes_key(const char *SecretKey)
{
    if (NULL == SecretKey) {
       return FALSE;
    }
    AES_KEY = SecretKey;
    log_debug("innet %s", AES_KEY);
    return TRUE;
}

const char *jd_innet_get_aes_key(void)
{
    return (const char *)AES_KEY;
}

