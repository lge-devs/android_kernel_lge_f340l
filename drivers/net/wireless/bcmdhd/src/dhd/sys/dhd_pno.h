/*
 * Header file of Broadcom Dongle Host Driver (DHD)
 * Prefered Network Offload code and Wi-Fi Location Service(WLS) code.
 * Copyright (C) 1999-2015, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dhd_pno.h 423669 2013-09-12 23:01:55Z $
 */

#ifndef __DHD_PNO_H__
#define __DHD_PNO_H__

#define PNO_TLV_PREFIX			'S'
#define PNO_TLV_VERSION			'1'
#define PNO_TLV_SUBTYPE_LEGACY_PNO '2'
#define PNO_TLV_RESERVED		'0'

#define PNO_BATCHING_SET "SET"
#define PNO_BATCHING_GET "GET"
#define PNO_BATCHING_STOP "STOP"

#define PNO_PARAMS_DELIMETER " "
#define PNO_PARAM_CHANNEL_DELIMETER ","
#define PNO_PARAM_VALUE_DELLIMETER '='
#define PNO_PARAM_SCANFREQ "SCANFREQ"
#define PNO_PARAM_BESTN	"BESTN"
#define PNO_PARAM_MSCAN "MSCAN"
#define PNO_PARAM_CHANNEL "CHANNEL"
#define PNO_PARAM_RTT "RTT"

#define PNO_TLV_TYPE_SSID_IE		'S'
#define PNO_TLV_TYPE_TIME		'T'
#define PNO_TLV_FREQ_REPEAT		'R'
#define PNO_TLV_FREQ_EXPO_MAX		'M'

#define MAXNUM_SSID_PER_ADD	16
#define MAXNUM_PNO_PARAMS 2
#define PNO_TLV_COMMON_LENGTH	1
#define DEFAULT_BATCH_MSCAN 16

#define RESULTS_END_MARKER "----\n"
#define SCAN_END_MARKER "####\n"
#define AP_END_MARKER "====\n"
#define GSCAN_MAX_CHANNELS_IN_BUCKET     32
#define GSCAN_MAX_AP_CACHE_PER_SCAN      32
#define GSCAN_MAX_AP_CACHE               320

enum scan_status {
	/* SCAN ABORT by other scan */
	PNO_STATUS_ABORT,
	/* RTT is presence or not */
	PNO_STATUS_RTT_PRESENCE,
	/* Disable PNO by Driver */
	PNO_STATUS_DISABLE,
	/* NORMAL BATCHING GET */
	PNO_STATUS_NORMAL,
	/* WLC_E_PFN_BEST_BATCHING */
	PNO_STATUS_EVENT,
	PNO_STATUS_MAX
};
#define PNO_STATUS_ABORT_MASK 0x0001
#define PNO_STATUS_RTT_MASK 0x0002
#define PNO_STATUS_DISABLE_MASK 0x0004
#define PNO_STATUS_OOM_MASK 0x0010

enum index_mode {
	INDEX_OF_LEGACY_PARAMS,
	INDEX_OF_BATCH_PARAMS,
	INDEX_OF_HOTLIST_PARAMS,
	INDEX_MODE_MAX
};
enum dhd_pno_status {
	DHD_PNO_DISABLED,
	DHD_PNO_ENABLED,
	DHD_PNO_SUSPEND
};
typedef struct cmd_tlv {
	char prefix;
	char version;
	char subtype;
	char reserved;
} cmd_tlv_t;
typedef enum dhd_pno_mode {
	/* Wi-Fi Legacy PNO Mode */
	DHD_PNO_NONE_MODE 	= 0,
	DHD_PNO_LEGACY_MODE = (1 << (0)),
	/* Wi-Fi Android BATCH SCAN Mode */
	DHD_PNO_BATCH_MODE = (1 << (1)),
	/* Wi-Fi Android Hotlist SCAN Mode */
	DHD_PNO_HOTLIST_MODE = (1 << (2))
} dhd_pno_mode_t;
struct dhd_pno_ssid {
	uint32		SSID_len;
	uchar		SSID[DOT11_MAX_SSID_LEN];
	struct list_head list;
};
struct dhd_pno_bssid {
	struct ether_addr	macaddr;
	/* Bit4: suppress_lost, Bit3: suppress_found */
	uint16			flags;
	struct list_head list;
};
typedef struct dhd_pno_bestnet_entry {
	struct ether_addr BSSID;
	uint8	SSID_len;
	uint8	SSID[DOT11_MAX_SSID_LEN];
	int8	RSSI;
	uint8	channel;
	uint32	timestamp;
	uint16	rtt0; /* distance_cm based on RTT */
	uint16	rtt1; /* distance_cm based on sample standard deviation */
	unsigned long recorded_time;
	struct list_head list;
} dhd_pno_bestnet_entry_t;
#define BESTNET_ENTRY_SIZE (sizeof(dhd_pno_bestnet_entry_t))

typedef struct dhd_pno_bestnet_header {
	struct dhd_pno_bestnet_header *next;
	uint8 reason;
	uint32 tot_cnt;
	uint32 tot_size;
	struct list_head entry_list;
} dhd_pno_best_header_t;
#define BEST_HEADER_SIZE (sizeof(dhd_pno_best_header_t))

typedef struct dhd_pno_scan_results {
	dhd_pno_best_header_t *bestnetheader;
	uint8 cnt_header;
	struct list_head list;
} dhd_pno_scan_results_t;
#define SCAN_RESULTS_SIZE (sizeof(dhd_pno_scan_results_t))

struct dhd_pno_get_batch_info {
	/* info related to get batch */
	char *buf;
	bool batch_started;
	uint32 tot_scan_cnt;
	uint32 expired_tot_scan_cnt;
	uint32 top_node_cnt;
	uint32 bufsize;
	uint32 bytes_written;
	int reason;
	struct list_head scan_results_list;
	struct list_head expired_scan_results_list;
};
struct dhd_pno_legacy_params {
	uint16 scan_fr;
	uint16 chan_list[WL_NUMCHANNELS];
	uint16 nchan;
	int pno_repeat;
	int pno_freq_expo_max;
	int nssid;
	struct list_head ssid_list;
};
struct dhd_pno_batch_params {
	int32 scan_fr;
	uint8 bestn;
	uint8 mscan;
	uint8 band;
	uint16 chan_list[WL_NUMCHANNELS];
	uint16 nchan;
	uint16 rtt;
	struct dhd_pno_get_batch_info get_batch;
};
struct dhd_pno_hotlist_params {
	uint8 band;
	int32 scan_fr;
	uint16 chan_list[WL_NUMCHANNELS];
	uint16 nchan;
	uint16 nbssid;
	struct list_head bssid_list;
};
#ifdef GSCAN_SUPPORT
typedef struct dhd_pno_gscan_channel_bucket {
	uint16 bucket_freq_multiple;
	/* band = 1 All bg band channels,
	 * band = 2 All a band channels,
	 * band = 0 chan_list channels
	 */
	uint16 band;
	uint8 report_flag;
	uint8 num_channels;
	uint16 chan_list[GSCAN_MAX_CH_BUCKETS];
} dhd_pno_gscan_channel_bucket_t;

struct dhd_pno_swc_evt_param {
	uint16 results_rxed_so_far;
	wl_pfn_significant_net_t *change_array;
};

typedef struct wifi_gscan_result {
    uint64 ts;                           /* Time of discovery           */
    char ssid[DOT11_MAX_SSID_LEN+1];     /* null terminated             */
    struct ether_addr	macaddr;         /* BSSID                      */
    uint32 channel;                      /* channel frequency in MHz    */
    int32 rssi;                          /* in db                       */
    uint64 rtt;                          /* in nanoseconds              */
    uint64 rtt_sd;                       /* standard deviation in rtt   */
    uint16 beacon_period;                /* units are Kusec             */
    uint16 capability;		            /* Capability information       */
    uint32 ie_length;		            /* byte length of Information Elements */
    char  ie_data[1];					/* IE data to follow       */
} wifi_gscan_result_t;

typedef struct gscan_results_cache {
	struct gscan_results_cache *next;
	uint8  scan_id;
	uint8  flag;
	uint8  tot_count;
	uint8  tot_consumed;
	wifi_gscan_result_t results[1];
} gscan_results_cache_t;

typedef struct dhd_pno_gscan_capabilities {
    int max_scan_cache_size;
    int max_scan_buckets;
    int max_ap_cache_per_scan;
    int max_rssi_sample_size;
    int max_scan_reporting_threshold;
    int max_hotlist_bssids;
    int max_hotlist_ssids;
    int max_significant_wifi_change_aps;
    int max_bssid_history_entries;
} dhd_pno_gscan_capabilities_t;

struct dhd_pno_gscan_params {
	int32 scan_fr;
	uint8 bestn;
	uint8 mscan;
	uint8 buffer_threshold;
	uint8 swc_nbssid_threshold;
	uint8 swc_rssi_window_size;
	uint8 lost_ap_window;
	uint8 nchannel_buckets;
	uint8 reason;
	uint8 get_batch_flag;
	uint8 send_all_results_flag;
	uint16 max_ch_bucket_freq;
	gscan_results_cache_t *gscan_batch_cache;
	gscan_results_cache_t *gscan_hotlist_found;
	gscan_results_cache_t *gscan_hotlist_lost;
	uint16 nbssid_significant_change;
	uint16 nbssid_hotlist;
	struct dhd_pno_swc_evt_param param_significant;
	struct dhd_pno_gscan_channel_bucket channel_bucket[GSCAN_MAX_CH_BUCKETS];
	struct list_head hotlist_bssid_list;
	struct list_head significant_bssid_list;
};

typedef struct gscan_scan_params {
	int32 scan_fr;
	uint16 nchannel_buckets;
	struct dhd_pno_gscan_channel_bucket channel_bucket[GSCAN_MAX_CH_BUCKETS];
} gscan_scan_params_t;

typedef struct gscan_batch_params {
	uint8 bestn;
	uint8 mscan;
	uint8 buffer_threshold;
} gscan_batch_params_t;

struct bssid_t {
	struct ether_addr	macaddr;
	int16 rssi_reporting_threshold;  /* 0 -> no reporting threshold */
};

typedef struct gscan_hotlist_scan_params {
	uint16 lost_ap_window; /* number of scans to declare LOST */
	uint16 nbssid;   /* number of bssids  */
	struct bssid_t bssid[1];  /* n bssids to follow */
} gscan_hotlist_scan_params_t;

#endif /* GSCAN_SUPPORT */

typedef union dhd_pno_params {
	struct dhd_pno_legacy_params params_legacy;
	struct dhd_pno_batch_params params_batch;
	struct dhd_pno_hotlist_params params_hotlist;
} dhd_pno_params_t;
typedef struct dhd_pno_status_info {
	dhd_pub_t *dhd;
	struct work_struct work;
	struct mutex pno_mutex;
	struct completion get_batch_done;
	bool wls_supported; /* wifi location service supported or not */
	enum dhd_pno_status pno_status;
	enum dhd_pno_mode pno_mode;
	dhd_pno_params_t pno_params_arr[INDEX_MODE_MAX];
	struct list_head head_list;
} dhd_pno_status_info_t;

/* wrapper functions */
extern int
dhd_dev_pno_enable(struct net_device *dev, int enable);

extern int
dhd_dev_pno_stop_for_ssid(struct net_device *dev);

extern int
dhd_dev_pno_set_for_ssid(struct net_device *dev, wlc_ssid_t* ssids_local, int nssid,
	uint16 scan_fr, int pno_repeat, int pno_freq_expo_max, uint16 *channel_list, int nchan);

extern int
dhd_dev_pno_set_for_batch(struct net_device *dev,
	struct dhd_pno_batch_params *batch_params);

extern int
dhd_dev_pno_get_for_batch(struct net_device *dev, char *buf, int bufsize);

extern int
dhd_dev_pno_stop_for_batch(struct net_device *dev);

extern int
dhd_dev_pno_set_for_hotlist(struct net_device *dev, wl_pfn_bssid_t *p_pfn_bssid,
	struct dhd_pno_hotlist_params *hotlist_params);

#ifdef LPS_SUPPORT
extern int
dhd_dev_pno_stop_for_hotlist(struct net_device *dev);
#endif	/* LPS_SUPPORT */
/* dhd pno fuctions */
extern int dhd_pno_stop_for_ssid(dhd_pub_t *dhd);
extern int dhd_pno_enable(dhd_pub_t *dhd, int enable);
extern int dhd_pno_set_for_ssid(dhd_pub_t *dhd, wlc_ssid_t* ssid_list, int nssid,
	uint16  scan_fr, int pno_repeat, int pno_freq_expo_max, uint16 *channel_list, int nchan);

extern int dhd_pno_set_for_batch(dhd_pub_t *dhd, struct dhd_pno_batch_params *batch_params);

extern int dhd_pno_get_for_batch(dhd_pub_t *dhd, char *buf, int bufsize, int reason);


extern int dhd_pno_stop_for_batch(dhd_pub_t *dhd);

extern int dhd_pno_set_for_hotlist(dhd_pub_t *dhd, wl_pfn_bssid_t *p_pfn_bssid,
	struct dhd_pno_hotlist_params *hotlist_params);

extern int dhd_pno_stop_for_hotlist(dhd_pub_t *dhd);

extern int dhd_pno_event_handler(dhd_pub_t *dhd, wl_event_msg_t *event, void *event_data);
extern int dhd_pno_init(dhd_pub_t *dhd);
extern int dhd_pno_deinit(dhd_pub_t *dhd);
#endif /* __DHD_PNO_H__ */
