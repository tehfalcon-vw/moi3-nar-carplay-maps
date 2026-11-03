/*
 * altscreen_handler.c v6.1 — Genuine CarPlay AltScreen Gen-2 Cluster Streamer
 * =============================================================================
 * Arquitetura de Producao IRC-Parity (MIB3 MOI3 LG AArch64 / Linux glibc 2.17+):
 *
 * 1. Display 0 (Central Multimidia 784x468):
 *    - 100% nativo de fabrica (Cinemo / libCarPlayMediaFramework.so).
 *    - Nao sofre interferencia de AirPlayInfoArrayAddScreenDisplay nem de contadores.
 *
 * 2. Anuncio de Displays (RTSP GET /info):
 *    - Hook em AirPlayCopyServerInfo(session, properties, outErr) [Assinatura exata AArch64 de 3 parametros].
 *    - Habilita Bit 26 (AltScreen) em root 'features' (features |= 1ULL << 26).
 *    - Adiciona root 'enabledFeatures': ["altScreen", "viewAreas"].
 *    - Clona stock display[0], remove chaves de touchscreen/HID (primaryInputDevice, edid, etc.)
 *      e define:
 *        type = 111 (K_STREAM_ALT / Instrument Cluster)
 *        maxFPS = 30
 *        features = 0 (View-only, passivo MFi)
 *        widthPixels = 1280, heightPixels = 480
 *        widthPhysical = 320, heightPhysical = 120
 *        uuid = "b7e6c5a0-2222-4000-8000-000000000002"
 *        initialURL = "maps:/car/instrumentcluster"
 *        altScreenSuggestUIURLs = ["maps:/car/instrumentcluster", ...]
 *
 * 3. Captura Criptografica de Sessao MFi:
 *    - Hook em AES_CBCFrame_Init (libCoreUtils.so).
 *    - Captura a chave mestre de 128 bits negociada no handshake AirPlay.
 *
 * 4. Negociacao de Stream (RTSP SETUP):
 *    - Hook em AirPlayReceiverSessionSetup (libAirPlay.so).
 *    - Detecta solicitacao de Stream 111 (Instrument Cluster H.264).
 *    - Deriva a subchave e IV via AirPlay_DeriveAESKeySHA512ForScreen(master_key, streamConnectionID).
 *    - Inicializa AES-CTR para decriptografia dos quadros de video.
 *    - Inicia servidor TCP local na porta 6031.
 *    - Anexa descritor de resposta do Stream 111 (dataPort = 6031, streamID = 111).
 *
 * 5. Receptor TCP & Pipeline MPEG-TS AID:
 *    - Thread alt_receiver_thread escuta em 0.0.0.0:6031 / [::]:6031.
 *    - Recebe pacotes de tela AirPlay (cabecalho 128B + payload).
 *    - K_SCREEN_VIDEO_CONFIG (opcode 1): extrai SPS e PPS da estrutura avcC.
 *    - K_SCREEN_VIDEO_FRAME (opcode 0): decripta com AES-CTR, converte AVCC para Annex B,
 *      empacota em MPEG-TS (PIDs 100/1000/17) e envia via UDP RTP multicast para
 *      o painel digital AID em [ff14::2:4]:42800 (eth0.5).
 * =============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#define LOG_FILE_DATA       "/data/lge/smartlink/carplay/carplay-handler.log"
#define LOG_FILE_APPRW      "/lge/app_rw/smartlink/carplay/carplay-handler.log"
#define LOG_FILE_FALLBACK   "/tmp/carplay-handler.log"

static FILE *g_log_fp = NULL;
static int g_kmsg_fd = -1;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_msg(const char *tag, const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_kmsg_fd < 0) {
        g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    }
    if (g_kmsg_fd >= 0) {
        char kmsg_buf[1152];
        snprintf(kmsg_buf, sizeof(kmsg_buf), "<3>[ALTSCREEN][%s] %s\n", tag, buffer);
        ssize_t wr = write(g_kmsg_fd, kmsg_buf, strlen(kmsg_buf));
        (void)wr;
    }

    if (!g_log_fp) {
        g_log_fp = fopen(LOG_FILE_DATA, "a");
        if (!g_log_fp) g_log_fp = fopen(LOG_FILE_APPRW, "a");
        if (!g_log_fp) g_log_fp = fopen(LOG_FILE_FALLBACK, "a");
    }
    if (g_log_fp) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
        fprintf(g_log_fp, "[%s] [%s] %s\n", time_str, tag, buffer);
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/* =============================================================================
 * Constantes do Protocolo AltScreen & Hardware Demuxer AID
 * ============================================================================= */
#define K_STREAM_ALT                 111
#define ALT_UUID_DEFAULT             "b7e6c5a0-2222-4000-8000-000000000002"
#define ALT_URL_DEFAULT              "maps:/car/instrumentcluster"
#define ALT_URL_MAP                  "maps:/car/instrumentcluster/map"
#define ALT_URL_INSTRUCTIONCARD      "maps:/car/instrumentcluster/instructioncard"
#define AIRPLAY_FEATURE_BIT26        (1ULL << 26)

#define K_SCREEN_HDR_SIZE            128
#define K_SCREEN_VIDEO_FRAME         0
#define K_SCREEN_VIDEO_CONFIG        1
#define K_SCREEN_KEEPALIVE           2
#define MAX_SCREEN_BODY              (8u * 1024u * 1024u)

#define CLUSTER_MULTICAST_IP         "ff14::2:4"
#define CLUSTER_MULTICAST_PORT       42800
#define RTP_PACKET_SIZE              1140
#define TS_PACKET_SIZE               188
#define CLUSTER_PROGRAM_NUMBER       100   /* 0x0064 */
#define CLUSTER_PMT_PID              1000  /* 0x03E8 */
#define CLUSTER_VIDEO_PID            17    /* 0x0011 */
#define CLUSTER_PCR_PID              17    /* 0x0011 */

typedef struct {
    uint32_t bodySize;
    uint8_t opcode;
    uint8_t smallParam[3];
    uint8_t params[15 * 8];
} AirPlayScreenHeaderCompat;

/* =============================================================================
 * Simbolos Dinamicos CoreFoundation & CoreUtils
 * ============================================================================= */
typedef const void *CFTypeRef;
typedef int64_t (*CFArrayGetCount_fn)(const void *array);
typedef const void *(*CFArrayGetValueAtIndex_fn)(const void *array, int64_t idx);
typedef void *(*CFArrayCreateMutable_fn)(const void *alloc, int64_t capacity, const void *callbacks);
typedef void *(*CFArrayCreateMutableCopy_fn)(const void *alloc, int64_t capacity, const void *array);
typedef void (*CFArrayAppendValue_fn)(void *array, const void *val);
typedef int32_t (*CFArrayEnsureCreatedAndAppendCString_fn)(void **ioArray, const char *str, size_t len);

typedef const void *(*CFDictionaryGetValue_fn)(const void *dict, const void *key);
typedef void (*CFDictionarySetValue_fn)(void *dict, const void *key, const void *val);
typedef void (*CFDictionaryRemoveValue_fn)(void *dict, const void *key);
typedef void (*CFDictionarySetInt64_fn)(void *dict, const void *key, int64_t val);
typedef int64_t (*CFDictionaryGetInt64_fn)(const void *dict, const void *key, int32_t *outErr);
typedef void *(*CFDictionaryCreateMutable_fn)(const void *alloc, int64_t capacity, const void *kc, const void *vc);
typedef void *(*CFDictionaryCreateMutableCopy_fn)(const void *alloc, int64_t capacity, const void *dict);

typedef void *(*CFStringCreateWithCString_fn)(const void *alloc, const char *cStr, uint32_t encoding);
typedef int (*CFStringGetCString_fn)(const void *theString, char *buffer, int64_t bufferSize, uint32_t encoding);
typedef void (*CFRelease_fn)(CFTypeRef cf);
typedef void *(*CFRetain_fn)(CFTypeRef cf);

static CFArrayGetCount_fn s_CFArrayGetCount = NULL;
static CFArrayGetValueAtIndex_fn s_CFArrayGetValueAtIndex = NULL;
static CFArrayCreateMutable_fn s_CFArrayCreateMutable = NULL;
static CFArrayCreateMutableCopy_fn s_CFArrayCreateMutableCopy = NULL;
static CFArrayAppendValue_fn s_CFArrayAppendValue = NULL;
static CFArrayEnsureCreatedAndAppendCString_fn s_CFArrayEnsureCreatedAndAppendCString = NULL;

static CFDictionaryGetValue_fn s_CFDictionaryGetValue = NULL;
static CFDictionarySetValue_fn s_CFDictionarySetValue = NULL;
static CFDictionaryRemoveValue_fn s_CFDictionaryRemoveValue = NULL;
static CFDictionarySetInt64_fn s_CFDictionarySetInt64 = NULL;
static CFDictionaryGetInt64_fn s_CFDictionaryGetInt64 = NULL;
static CFDictionaryCreateMutable_fn s_CFDictionaryCreateMutable = NULL;
static CFDictionaryCreateMutableCopy_fn s_CFDictionaryCreateMutableCopy = NULL;

static CFStringCreateWithCString_fn s_CFStringCreateWithCString = NULL;
static CFStringGetCString_fn s_CFStringGetCString = NULL;
static CFRelease_fn s_CFRelease = NULL;
static CFRetain_fn s_CFRetain = NULL;

typedef int32_t (*CFPropertyListWriteToFilePath_fn)(void *plist, const char *path, ...);
static CFPropertyListWriteToFilePath_fn s_CFPropertyListWriteToFilePath = NULL;
static const void *s_kCFLBooleanFalse = NULL;

static const void *s_kCFLArrayCallBacks = NULL;
static const void *s_kCFLDictKeyCallBacks = NULL;
static const void *s_kCFLDictValCallBacks = NULL;

static void init_cf_symbols(void) {
    if (s_CFArrayGetCount) return;
    s_CFArrayGetCount = (CFArrayGetCount_fn)dlsym(RTLD_DEFAULT, "CFArrayGetCount");
    s_CFArrayGetValueAtIndex = (CFArrayGetValueAtIndex_fn)dlsym(RTLD_DEFAULT, "CFArrayGetValueAtIndex");
    s_CFArrayCreateMutable = (CFArrayCreateMutable_fn)dlsym(RTLD_DEFAULT, "CFArrayCreateMutable");
    s_CFArrayCreateMutableCopy = (CFArrayCreateMutableCopy_fn)dlsym(RTLD_DEFAULT, "CFArrayCreateMutableCopy");
    s_CFArrayAppendValue = (CFArrayAppendValue_fn)dlsym(RTLD_DEFAULT, "CFArrayAppendValue");
    s_CFArrayEnsureCreatedAndAppendCString = (CFArrayEnsureCreatedAndAppendCString_fn)dlsym(
        RTLD_DEFAULT, "CFArrayEnsureCreatedAndAppendCString"
    );

    s_CFDictionaryGetValue = (CFDictionaryGetValue_fn)dlsym(RTLD_DEFAULT, "CFDictionaryGetValue");
    s_CFDictionarySetValue = (CFDictionarySetValue_fn)dlsym(RTLD_DEFAULT, "CFDictionarySetValue");
    s_CFDictionaryRemoveValue = (CFDictionaryRemoveValue_fn)dlsym(RTLD_DEFAULT, "CFDictionaryRemoveValue");
    s_CFDictionarySetInt64 = (CFDictionarySetInt64_fn)dlsym(RTLD_DEFAULT, "CFDictionarySetInt64");
    s_CFDictionaryGetInt64 = (CFDictionaryGetInt64_fn)dlsym(RTLD_DEFAULT, "CFDictionaryGetInt64");
    s_CFDictionaryCreateMutable = (CFDictionaryCreateMutable_fn)dlsym(RTLD_DEFAULT, "CFDictionaryCreateMutable");
    s_CFDictionaryCreateMutableCopy = (CFDictionaryCreateMutableCopy_fn)dlsym(RTLD_DEFAULT, "CFDictionaryCreateMutableCopy");

    s_CFStringCreateWithCString = (CFStringCreateWithCString_fn)dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    s_CFStringGetCString = (CFStringGetCString_fn)dlsym(RTLD_DEFAULT, "CFStringGetCString");
    s_CFRelease = (CFRelease_fn)dlsym(RTLD_DEFAULT, "CFRelease");
    s_CFRetain = (CFRetain_fn)dlsym(RTLD_DEFAULT, "CFRetain");

    s_CFPropertyListWriteToFilePath = (CFPropertyListWriteToFilePath_fn)dlsym(
        RTLD_DEFAULT, "CFPropertyListWriteToFilePath"
    );
    void **false_slot = (void **)dlsym(RTLD_DEFAULT, "kCFLBooleanFalse");
    s_kCFLBooleanFalse = false_slot ? *false_slot : NULL;

    s_kCFLArrayCallBacks = dlsym(RTLD_DEFAULT, "kCFLArrayCallBacksCFLTypes");
    s_kCFLDictKeyCallBacks = dlsym(RTLD_DEFAULT, "kCFLDictionaryKeyCallBacksCFLTypes");
    s_kCFLDictValCallBacks = dlsym(RTLD_DEFAULT, "kCFLDictionaryValueCallBacksCFLTypes");
}

static void *s_cf(const char *s) {
    if (!s || !s_CFStringCreateWithCString) return NULL;
    return s_CFStringCreateWithCString(NULL, s, 0x08000100 /* UTF-8 */);
}

static void set_str(void *dict, const char *key, const char *val) {
    void *k = s_cf(key);
    void *v = s_cf(val);
    if (k && v && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(dict, k, v);
    }
    if (v && s_CFRelease) s_CFRelease(v);
    if (k && s_CFRelease) s_CFRelease(k);
}

static void set_i64(void *dict, const char *key, int64_t val) {
    void *k = s_cf(key);
    if (k && s_CFDictionarySetInt64) {
        s_CFDictionarySetInt64(dict, k, val);
    }
    if (k && s_CFRelease) s_CFRelease(k);
}

static void remove_key(void *dict, const char *key) {
    void *k = s_cf(key);
    if (k && s_CFDictionaryRemoveValue) {
        s_CFDictionaryRemoveValue(dict, k);
    }
    if (k && s_CFRelease) s_CFRelease(k);
}

static void *dict_clone(void *dict) {
    if (!dict || !s_CFDictionaryCreateMutableCopy) return NULL;
    return s_CFDictionaryCreateMutableCopy(NULL, 0, dict);
}

static void *array_clone(void *arr) {
    if (!arr || !s_CFArrayCreateMutableCopy) return NULL;
    return s_CFArrayCreateMutableCopy(NULL, 0, arr);
}

/* =============================================================================
 * ViewAreas e enabledFeatures (AltScreen Apple CarPlay Specification)
 * ============================================================================= */
static int check_viewareas_enabled(void) {
    if (access("/data/lge/smartlink/carplay/ENABLE_VIEWAREAS", F_OK) == 0) return 1;
    if (access("/tmp/USB/PORT1/PART1/ENABLE_VIEWAREAS.txt", F_OK) == 0) return 1;
    if (access("/tmp/usbmnt/ENABLE_VIEWAREAS.txt", F_OK) == 0) return 1;
    return 0; /* Padrao 0: modo nativo altScreen limpo e fail-safe sem viewAreas */
}

static void set_reference_enabled_features(void *dict, int enable_viewareas) {
    if (!dict || !s_CFArrayEnsureCreatedAndAppendCString) return;
    void *enabled_features = NULL;
    s_CFArrayEnsureCreatedAndAppendCString(&enabled_features, "altScreen", (size_t)-1);
    if (enable_viewareas) {
        s_CFArrayEnsureCreatedAndAppendCString(&enabled_features, "viewAreas", (size_t)-1);
    }
    void *k_enabled = s_cf("enabledFeatures");
    if (k_enabled && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(dict, k_enabled, enabled_features);
        if (s_CFRelease) s_CFRelease(k_enabled);
    }
    if (enabled_features && s_CFRelease) s_CFRelease(enabled_features);
    log_msg("SERVERINFO", "enabledFeatures configurado com ['altScreen'%s]", enable_viewareas ? ", 'viewAreas'" : "");
}

static void add_reference_viewarea(void *alt) {
    if (!alt || !s_CFDictionaryCreateMutable || !s_CFArrayCreateMutable || !s_kCFLBooleanFalse) {
        log_msg("SERVERINFO", "add_reference_viewarea ignorado: simbolos CFLite boolean/dict incompletos");
        return;
    }

    void *view = s_CFDictionaryCreateMutable(NULL, 0, s_kCFLDictKeyCallBacks, s_kCFLDictValCallBacks);
    void *safe = s_CFDictionaryCreateMutable(NULL, 0, s_kCFLDictKeyCallBacks, s_kCFLDictValCallBacks);
    void *areas = s_CFArrayCreateMutable(NULL, 0, s_kCFLArrayCallBacks);
    if (!view || !safe || !areas) {
        if (view && s_CFRelease) s_CFRelease(view);
        if (safe && s_CFRelease) s_CFRelease(safe);
        if (areas && s_CFRelease) s_CFRelease(areas);
        return;
    }

    set_i64(view, "widthPixels", 1280);
    set_i64(view, "heightPixels", 480);
    set_i64(view, "originXPixels", 0);
    set_i64(view, "originYPixels", 0);

    set_i64(safe, "widthPixels", 1280);
    set_i64(safe, "heightPixels", 480);
    set_i64(safe, "originXPixels", 0);
    set_i64(safe, "originYPixels", 0);

    void *k_safe = s_cf("safeArea");
    if (k_safe && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(view, k_safe, safe);
        if (s_CFRelease) s_CFRelease(k_safe);
    }

    void *k_drawUI = s_cf("drawUIOutsideSafeArea");
    if (k_drawUI && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(view, k_drawUI, s_kCFLBooleanFalse);
        if (s_CFRelease) s_CFRelease(k_drawUI);
    }

    void *k_trans = s_cf("viewAreaTransitionControl");
    if (k_trans && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(view, k_trans, s_kCFLBooleanFalse);
        if (s_CFRelease) s_CFRelease(k_trans);
    }

    if (s_CFArrayAppendValue) {
        s_CFArrayAppendValue(areas, view);
    }

    void *k_va = s_cf("viewAreas");
    if (k_va && s_CFDictionarySetValue) {
        s_CFDictionarySetValue(alt, k_va, areas);
        if (s_CFRelease) s_CFRelease(k_va);
    }

    set_i64(alt, "initialViewArea", 0);
    log_msg("SERVERINFO", "ViewAreas configurado: 1280x480 (safeArea=1280x480@0,0, initialViewArea=0)");

    if (areas && s_CFRelease) s_CFRelease(areas);
    if (safe && s_CFRelease) s_CFRelease(safe);
    if (view && s_CFRelease) s_CFRelease(view);
}

/* =============================================================================
 * Criptografia CarPlay MFi (AES-128-CTR Derivada de Sessao)
 * ============================================================================= */
typedef union {
    uint64_t align;
    unsigned char bytes[512];
} aes_ctr_storage_t;

typedef int32_t (*AirPlay_DeriveAESKeySHA512ForScreen_fn)(
    const uint8_t *master_key, size_t key_len, uint64_t connection_id, uint8_t *out_key, uint8_t *out_iv
);
typedef int32_t (*AES_CTR_Init_fn)(void *ctx, const uint8_t *key, const uint8_t *iv);
typedef int32_t (*AES_CTR_Update_fn)(void *ctx, const void *src, size_t len, void *dst);

static AirPlay_DeriveAESKeySHA512ForScreen_fn s_AirPlay_DeriveAESKeySHA512ForScreen = NULL;
static AES_CTR_Init_fn s_AES_CTR_Init = NULL;
static AES_CTR_Update_fn s_AES_CTR_Update = NULL;

static uint8_t g_master_key[16];
static volatile int g_master_valid = 0;
static pthread_mutex_t g_crypto_lock = PTHREAD_MUTEX_INITIALIZER;

static aes_ctr_storage_t g_alt_aes;
static volatile int g_alt_aes_valid = 0;

static void init_crypto_symbols(void) {
    if (s_AirPlay_DeriveAESKeySHA512ForScreen) return;
    s_AirPlay_DeriveAESKeySHA512ForScreen = (AirPlay_DeriveAESKeySHA512ForScreen_fn)dlsym(
        RTLD_DEFAULT, "AirPlay_DeriveAESKeySHA512ForScreen"
    );
    s_AES_CTR_Init = (AES_CTR_Init_fn)dlsym(RTLD_DEFAULT, "AES_CTR_Init");
    s_AES_CTR_Update = (AES_CTR_Update_fn)dlsym(RTLD_DEFAULT, "AES_CTR_Update");
}

/* Hook em AES_CBCFrame_Init para interceptar a chave mestre de 128 bits no handshake AirPlay */
typedef int32_t (*AES_CBCFrame_Init_fn)(void *ctx, const uint8_t key[16], const uint8_t iv[16], int encrypt);
static AES_CBCFrame_Init_fn real_AES_CBCFrame_Init = NULL;

int32_t AES_CBCFrame_Init(void *ctx, const uint8_t key[16], const uint8_t iv[16], int encrypt) {
    if (!real_AES_CBCFrame_Init) {
        real_AES_CBCFrame_Init = (AES_CBCFrame_Init_fn)dlsym(RTLD_NEXT, "AES_CBCFrame_Init");
    }

    if (key && !encrypt) {
        pthread_mutex_lock(&g_crypto_lock);
        memcpy(g_master_key, key, 16);
        g_master_valid = 1;
        pthread_mutex_unlock(&g_crypto_lock);
        log_msg("CRYPTO", "Chave mestre CarPlay AES-128 capturada com sucesso!");
    }

    if (real_AES_CBCFrame_Init) {
        return real_AES_CBCFrame_Init(ctx, key, iv, encrypt);
    }
    return 0;
}

/* =============================================================================
 * Camada 3: MPEG-TS & RTP Multicast Pipeline para [ff14::2:4]:42800 (eth0.5)
 * PIDs Fixos de Hardware do AID: Program 100, PMT 1000, Video/PCR PID 17
 * ============================================================================= */
static int g_rtp_sock = -1;
static struct sockaddr_in6 g_cluster_dest_addr;
static uint16_t g_rtp_seq = 0;
static uint32_t g_rtp_ts = 0;

static uint8_t g_pat_cc = 0;
static uint8_t g_pmt_cc = 0;
static uint8_t g_video_cc = 0;

static uint8_t g_rtp_buf[RTP_PACKET_SIZE];
static int g_rtp_ts_in_buf = 0;

static uint64_t g_total_frames_sent = 0;
static uint64_t g_total_bytes_sent = 0;

static uint32_t ts_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc = crc << 1;
        }
    }
    return crc;
}

static void init_cluster_network(void) {
    if (g_rtp_sock >= 0) return;

    g_rtp_sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (g_rtp_sock < 0) {
        log_msg("NET_ERR", "Falha ao criar socket IPv6 RTP: %s", strerror(errno));
        return;
    }

    int hops = 5;
    setsockopt(g_rtp_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));

    int loop = 1;
    setsockopt(g_rtp_sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop));

    int prio = 3;
    setsockopt(g_rtp_sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));

    unsigned int ifindex = if_nametoindex("eth0.5");
    if (ifindex == 0) ifindex = if_nametoindex("eth0");
    if (ifindex > 0) {
        setsockopt(g_rtp_sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    }

    memset(&g_cluster_dest_addr, 0, sizeof(g_cluster_dest_addr));
    g_cluster_dest_addr.sin6_family = AF_INET6;
    g_cluster_dest_addr.sin6_port = htons(CLUSTER_MULTICAST_PORT);
    inet_pton(AF_INET6, CLUSTER_MULTICAST_IP, &g_cluster_dest_addr.sin6_addr);

    log_msg("NET", "Socket RTP inicializado para [%s]:%d (ifidx=%u, Prio=%d, PID %d, PMT %d, Prog %d)",
            CLUSTER_MULTICAST_IP, CLUSTER_MULTICAST_PORT, ifindex, prio,
            CLUSTER_VIDEO_PID, CLUSTER_PMT_PID, CLUSTER_PROGRAM_NUMBER);
}

static void rtp_flush_buffer(int marker) {
    if (g_rtp_ts_in_buf == 0 || g_rtp_sock < 0) return;

    g_rtp_buf[0] = 0x80;
    g_rtp_buf[1] = 33 | (marker ? 0x80 : 0x00);
    g_rtp_buf[2] = (uint8_t)(g_rtp_seq >> 8);
    g_rtp_buf[3] = (uint8_t)(g_rtp_seq & 0xFF);
    g_rtp_seq++;

    g_rtp_buf[4] = (uint8_t)((g_rtp_ts >> 24) & 0xFF);
    g_rtp_buf[5] = (uint8_t)((g_rtp_ts >> 16) & 0xFF);
    g_rtp_buf[6] = (uint8_t)((g_rtp_ts >> 8) & 0xFF);
    g_rtp_buf[7] = (uint8_t)(g_rtp_ts & 0xFF);

    g_rtp_buf[8]  = 0x41;
    g_rtp_buf[9]  = 0x49;
    g_rtp_buf[10] = 0x44;
    g_rtp_buf[11] = 0x31;

    size_t payload_size = 12 + (g_rtp_ts_in_buf * TS_PACKET_SIZE);
    ssize_t sent = sendto(g_rtp_sock, g_rtp_buf, payload_size, 0,
                          (struct sockaddr *)&g_cluster_dest_addr, sizeof(g_cluster_dest_addr));
    if (sent > 0) {
        g_total_bytes_sent += (uint64_t)sent;
    }
    g_rtp_ts_in_buf = 0;
}

static void rtp_queue_ts_packet(const uint8_t *ts_pkt) {
    memcpy(&g_rtp_buf[12 + (g_rtp_ts_in_buf * TS_PACKET_SIZE)], ts_pkt, TS_PACKET_SIZE);
    g_rtp_ts_in_buf++;
    if (g_rtp_ts_in_buf == 6) {
        rtp_flush_buffer(0);
    }
}

static void emit_pat_packet(void) {
    uint8_t pkt[TS_PACKET_SIZE];
    memset(pkt, 0xFF, TS_PACKET_SIZE);

    pkt[0] = 0x47;
    pkt[1] = 0x40;
    pkt[2] = 0x00;
    pkt[3] = 0x10 | (g_pat_cc & 0x0F);
    g_pat_cc = (g_pat_cc + 1) & 0x0F;

    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0xB0;
    pkt[7] = 0x0D;
    pkt[8] = 0x00; pkt[9] = 0x01;
    pkt[10] = 0xC1;
    pkt[11] = 0x00; pkt[12] = 0x00;

    pkt[13] = (uint8_t)((CLUSTER_PROGRAM_NUMBER >> 8) & 0xFF);
    pkt[14] = (uint8_t)(CLUSTER_PROGRAM_NUMBER & 0xFF);
    pkt[15] = 0xE0 | (uint8_t)((CLUSTER_PMT_PID >> 8) & 0x1F);
    pkt[16] = (uint8_t)(CLUSTER_PMT_PID & 0xFF);

    uint32_t crc = ts_crc32(&pkt[5], 12);
    pkt[17] = (uint8_t)((crc >> 24) & 0xFF);
    pkt[18] = (uint8_t)((crc >> 16) & 0xFF);
    pkt[19] = (uint8_t)((crc >> 8) & 0xFF);
    pkt[20] = (uint8_t)(crc & 0xFF);

    rtp_queue_ts_packet(pkt);
}

static void emit_pmt_packet(void) {
    uint8_t pkt[TS_PACKET_SIZE];
    memset(pkt, 0xFF, TS_PACKET_SIZE);

    pkt[0] = 0x47;
    pkt[1] = 0x40 | (uint8_t)((CLUSTER_PMT_PID >> 8) & 0x1F);
    pkt[2] = (uint8_t)(CLUSTER_PMT_PID & 0xFF);
    pkt[3] = 0x10 | (g_pmt_cc & 0x0F);
    g_pmt_cc = (g_pmt_cc + 1) & 0x0F;

    pkt[4] = 0x00;
    pkt[5] = 0x02;
    pkt[6] = 0xB0;
    pkt[7] = 0x12;
    pkt[8] = (uint8_t)((CLUSTER_PROGRAM_NUMBER >> 8) & 0xFF);
    pkt[9] = (uint8_t)(CLUSTER_PROGRAM_NUMBER & 0xFF);
    pkt[10] = 0xC1;
    pkt[11] = 0x00; pkt[12] = 0x00;

    pkt[13] = 0xE0 | (uint8_t)((CLUSTER_PCR_PID >> 8) & 0x1F);
    pkt[14] = (uint8_t)(CLUSTER_PCR_PID & 0xFF);
    pkt[15] = 0xF0; pkt[16] = 0x00;

    pkt[17] = 0x1B;
    pkt[18] = 0xE0 | (uint8_t)((CLUSTER_VIDEO_PID >> 8) & 0x1F);
    pkt[19] = (uint8_t)(CLUSTER_VIDEO_PID & 0xFF);
    pkt[20] = 0xF0; pkt[21] = 0x00;

    uint32_t crc = ts_crc32(&pkt[5], 17);
    pkt[22] = (uint8_t)((crc >> 24) & 0xFF);
    pkt[23] = (uint8_t)((crc >> 16) & 0xFF);
    pkt[24] = (uint8_t)((crc >> 8) & 0xFF);
    pkt[25] = (uint8_t)(crc & 0xFF);

    rtp_queue_ts_packet(pkt);
}

static void mux_and_stream_h264_frame(const uint8_t *frame_data, size_t frame_len, uint64_t pts_90k) {
    if (!frame_data || frame_len == 0) return;

    size_t pes_header_len = 14;
    size_t pes_total_len = pes_header_len + frame_len;
    uint8_t *pes_buf = (uint8_t *)malloc(pes_total_len);
    if (!pes_buf) return;

    pes_buf[0] = 0x00;
    pes_buf[1] = 0x00;
    pes_buf[2] = 0x01;
    pes_buf[3] = 0xE0;

    uint32_t pes_payload_len = (pes_total_len - 6 > 65535) ? 0 : (uint32_t)(pes_total_len - 6);
    pes_buf[4] = (uint8_t)((pes_payload_len >> 8) & 0xFF);
    pes_buf[5] = (uint8_t)(pes_payload_len & 0xFF);

    pes_buf[6] = 0x80;
    pes_buf[7] = 0x80;
    pes_buf[8] = 0x05;

    pes_buf[9]  = 0x20 | (uint8_t)(((pts_90k >> 30) & 0x07) << 1) | 0x01;
    pes_buf[10] = (uint8_t)((pts_90k >> 22) & 0xFF);
    pes_buf[11] = (uint8_t)(((pts_90k >> 15) & 0x7F) << 1) | 0x01;
    pes_buf[12] = (uint8_t)((pts_90k >> 7) & 0xFF);
    pes_buf[13] = (uint8_t)((pts_90k & 0x7F) << 1) | 0x01;

    memcpy(&pes_buf[pes_header_len], frame_data, frame_len);

    size_t offset = 0;
    int is_first_pkt = 1;

    while (offset < pes_total_len) {
        uint8_t ts_pkt[TS_PACKET_SIZE];
        memset(ts_pkt, 0xFF, TS_PACKET_SIZE);

        ts_pkt[0] = 0x47;
        ts_pkt[1] = (is_first_pkt ? 0x40 : 0x00) | (uint8_t)((CLUSTER_VIDEO_PID >> 8) & 0x1F);
        ts_pkt[2] = (uint8_t)(CLUSTER_VIDEO_PID & 0xFF);

        size_t remaining = pes_total_len - offset;

        if (is_first_pkt) {
            is_first_pkt = 0;
            ts_pkt[3] = 0x30 | (g_video_cc & 0x0F);
            g_video_cc = (g_video_cc + 1) & 0x0F;

            ts_pkt[4] = 0x07;
            ts_pkt[5] = 0x10;

            uint64_t pcr_base = pts_90k * 300;
            ts_pkt[6] = (uint8_t)((pcr_base >> 25) & 0xFF);
            ts_pkt[7] = (uint8_t)((pcr_base >> 17) & 0xFF);
            ts_pkt[8] = (uint8_t)((pcr_base >> 9) & 0xFF);
            ts_pkt[9] = (uint8_t)((pcr_base >> 1) & 0xFF);
            ts_pkt[10] = (uint8_t)(((pcr_base & 0x01) << 7) | 0x7E);
            ts_pkt[11] = 0x00;

            size_t max_body = TS_PACKET_SIZE - 12;
            size_t copy_len = (remaining < max_body) ? remaining : max_body;
            memcpy(&ts_pkt[12], pes_buf + offset, copy_len);
            offset += copy_len;

            if (copy_len < max_body) {
                size_t padding = max_body - copy_len;
                ts_pkt[4] += (uint8_t)padding;
                memmove(&ts_pkt[12 + padding], &ts_pkt[12], copy_len);
                memset(&ts_pkt[12], 0xFF, padding);
            }
        } else {
            if (remaining >= TS_PACKET_SIZE - 4) {
                ts_pkt[3] = 0x10 | (g_video_cc & 0x0F);
                g_video_cc = (g_video_cc + 1) & 0x0F;
                memcpy(&ts_pkt[4], pes_buf + offset, TS_PACKET_SIZE - 4);
                offset += (TS_PACKET_SIZE - 4);
            } else {
                ts_pkt[3] = 0x30 | (g_video_cc & 0x0F);
                g_video_cc = (g_video_cc + 1) & 0x0F;
                size_t stuff_len = (TS_PACKET_SIZE - 4) - remaining;
                if (stuff_len == 1) {
                    ts_pkt[4] = 0x00;
                    memcpy(&ts_pkt[5], pes_buf + offset, remaining);
                } else {
                    ts_pkt[4] = (uint8_t)(stuff_len - 1);
                    ts_pkt[5] = 0x00;
                    if (stuff_len > 2) memset(&ts_pkt[6], 0xFF, stuff_len - 2);
                    memcpy(&ts_pkt[4 + stuff_len], pes_buf + offset, remaining);
                }
                offset += remaining;
            }
        }
        rtp_queue_ts_packet(ts_pkt);
    }

    free(pes_buf);
    g_rtp_ts = (uint32_t)pts_90k;
    rtp_flush_buffer(1);
    g_total_frames_sent++;

    if (g_total_frames_sent <= 5 || g_total_frames_sent % 300 == 0) {
        log_msg("STREAM", "Stream AID: %llu frames transmitidos (%llu bytes) para [%s]:%d",
                (unsigned long long)g_total_frames_sent,
                (unsigned long long)g_total_bytes_sent,
                CLUSTER_MULTICAST_IP, CLUSTER_MULTICAST_PORT);
    }
}

/* =============================================================================
 * Parser AVCC & Conversor Annex B
 * ============================================================================= */
static uint8_t g_sps_pps_buf[512];
static size_t g_sps_pps_len = 0;

static void parse_and_store_avcc(const uint8_t *avcc, size_t len) {
    if (!avcc || len < 7) return;

    size_t out_pos = 0;
    uint8_t num_sps = avcc[5] & 0x1F;
    size_t pos = 6;

    for (int i = 0; i < num_sps && pos + 2 <= len; i++) {
        uint16_t sps_len = ((uint16_t)avcc[pos] << 8) | avcc[pos + 1];
        pos += 2;
        if (pos + sps_len > len) break;

        if (out_pos + 4 + sps_len < sizeof(g_sps_pps_buf)) {
            g_sps_pps_buf[out_pos++] = 0x00;
            g_sps_pps_buf[out_pos++] = 0x00;
            g_sps_pps_buf[out_pos++] = 0x00;
            g_sps_pps_buf[out_pos++] = 0x01;
            memcpy(&g_sps_pps_buf[out_pos], &avcc[pos], sps_len);
            out_pos += sps_len;
        }
        pos += sps_len;
    }

    if (pos < len) {
        uint8_t num_pps = avcc[pos++];
        for (int i = 0; i < num_pps && pos + 2 <= len; i++) {
            uint16_t pps_len = ((uint16_t)avcc[pos] << 8) | avcc[pos + 1];
            pos += 2;
            if (pos + pps_len > len) break;

            if (out_pos + 4 + pps_len < sizeof(g_sps_pps_buf)) {
                g_sps_pps_buf[out_pos++] = 0x00;
                g_sps_pps_buf[out_pos++] = 0x00;
                g_sps_pps_buf[out_pos++] = 0x00;
                g_sps_pps_buf[out_pos++] = 0x01;
                memcpy(&g_sps_pps_buf[out_pos], &avcc[pos], pps_len);
                out_pos += pps_len;
            }
            pos += pps_len;
        }
    }

    g_sps_pps_len = out_pos;
    log_msg("AVCC", "SPS/PPS extraidos com sucesso! Tamanho total: %zu bytes", g_sps_pps_len);
}

static int process_h264_frame(const uint8_t *data, size_t len, uint64_t timestamp) {
    if (!data || len < 4) return 0;

    init_cluster_network();

    static int s_first_frame = 0;
    if (!s_first_frame) {
        s_first_frame = 1;
        log_msg("STREAM111", ">>> PRIMEIRO QUADRO DO CLUSTER RECEBIDO (%zu bytes, ts=%llu)! Transmitindo...",
                len, (unsigned long long)timestamp);
    }

    size_t annexb_capacity = len + g_sps_pps_len + 64;
    uint8_t *annexb_frame = (uint8_t *)malloc(annexb_capacity);
    if (!annexb_frame) return 0;

    size_t annexb_pos = 0;
    int is_keyframe = 0;

    size_t in_pos = 0;
    while (in_pos + 4 <= len) {
        uint32_t nal_len = ((uint32_t)data[in_pos] << 24) |
                           ((uint32_t)data[in_pos + 1] << 16) |
                           ((uint32_t)data[in_pos + 2] << 8) |
                           ((uint32_t)data[in_pos + 3]);
        in_pos += 4;
        if (in_pos + nal_len > len) break;

        uint8_t nal_type = data[in_pos] & 0x1F;
        if (nal_type == 5) {
            is_keyframe = 1;
        }

        if (is_keyframe && annexb_pos == 0 && g_sps_pps_len > 0) {
            memcpy(&annexb_frame[annexb_pos], g_sps_pps_buf, g_sps_pps_len);
            annexb_pos += g_sps_pps_len;
        }

        annexb_frame[annexb_pos++] = 0x00;
        annexb_frame[annexb_pos++] = 0x00;
        annexb_frame[annexb_pos++] = 0x00;
        annexb_frame[annexb_pos++] = 0x01;

        memcpy(&annexb_frame[annexb_pos], &data[in_pos], nal_len);
        annexb_pos += nal_len;

        in_pos += nal_len;
    }

    static int s_pat_counter = 0;
    if (is_keyframe || (++s_pat_counter % 30 == 0)) {
        emit_pat_packet();
        emit_pmt_packet();
    }

    uint64_t pts_90k = timestamp ? (timestamp * 90 / 1000) : (g_total_frames_sent * 3000);
    mux_and_stream_h264_frame(annexb_frame, annexb_pos, pts_90k);

    free(annexb_frame);
    return 0;
}

/* =============================================================================
 * Servidor Receptor TCP Stream 111 (Porta 6031)
 * ============================================================================= */
static int g_alt_listen_fd = -1;
static int g_alt_client_fd = -1;
static pthread_t g_alt_thread;
static volatile int g_alt_thread_running = 0;

static int read_exact(int fd, void *buf, size_t len) {
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return (int)n;
        }
        total += (size_t)n;
    }
    return 1;
}

static void *alt_receiver_thread(void *arg) {
    int listen_fd = (int)(intptr_t)arg;
    log_msg("RECV111", "Aguardando conexao TCP do iPhone no Stream 111 (porta 6031)...");

    int c = accept(listen_fd, NULL, NULL);
    if (c < 0) {
        log_msg("RECV111", "Falha no accept do Stream 111: %s", strerror(errno));
        return NULL;
    }

    pthread_mutex_lock(&g_crypto_lock);
    g_alt_client_fd = c;
    pthread_mutex_unlock(&g_crypto_lock);
    log_msg("RECV111", ">>> IPHONE CONECTADO NO STREAM 111 (TCP fd=%d)! Recebendo quadros H.264...", c);

    while (g_alt_thread_running) {
        AirPlayScreenHeaderCompat h;
        uint8_t *body = NULL;

        int rr = read_exact(c, &h, sizeof(h));
        if (rr <= 0) {
            log_msg("RECV111", "Conexao TCP encerrada pelo iPhone (header rr=%d)", rr);
            break;
        }

        if (h.bodySize > MAX_SCREEN_BODY) {
            log_msg("RECV111", "bodySize invalido (%u bytes)", h.bodySize);
            break;
        }

        if (h.bodySize > 0) {
            body = (uint8_t *)malloc(h.bodySize);
            if (!body) break;
            rr = read_exact(c, body, h.bodySize);
            if (rr <= 0) {
                free(body);
                break;
            }
        }

        switch (h.opcode) {
            case K_SCREEN_VIDEO_CONFIG:
                if (body && h.bodySize > 0) {
                    log_msg("RECV111", "VideoConfig recebido: bodySize=%u bytes", h.bodySize);
                    parse_and_store_avcc(body, h.bodySize);
                }
                break;

            case K_SCREEN_VIDEO_FRAME:
                if (body && h.bodySize > 0) {
                    if (g_alt_aes_valid && s_AES_CTR_Update) {
                        s_AES_CTR_Update(&g_alt_aes, body, h.bodySize, body);
                    }
                    process_h264_frame(body, h.bodySize, 0);
                }
                break;

            case K_SCREEN_KEEPALIVE:
                break;

            default:
                break;
        }

        if (body) {
            free(body);
        }
    }

    log_msg("RECV111", "Stream 111 desconectado.");
    pthread_mutex_lock(&g_crypto_lock);
    if (g_alt_client_fd == c) g_alt_client_fd = -1;
    close(c);
    pthread_mutex_unlock(&g_crypto_lock);
    return NULL;
}

static int bind_listener_stream111(int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd >= 0) {
        int one = 1;
        int off = 0;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

        struct sockaddr_in6 sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_addr = in6addr_any;
        sa6.sin6_port = htons((uint16_t)port);

        if (bind(fd, (struct sockaddr *)&sa6, sizeof(sa6)) == 0 && listen(fd, 2) == 0) {
            return fd;
        }
        close(fd);
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa4;
    memset(&sa4, 0, sizeof(sa4));
    sa4.sin_family = AF_INET;
    sa4.sin_addr.s_addr = htonl(INADDR_ANY);
    sa4.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int start_alt_receiver(uint64_t connection_id) {
    pthread_mutex_lock(&g_crypto_lock);

    if (g_alt_listen_fd >= 0) {
        close(g_alt_listen_fd);
        g_alt_listen_fd = -1;
    }

    if (!g_master_valid) {
        pthread_mutex_unlock(&g_crypto_lock);
        log_msg("RECV111", "AVISO: Master AES key ainda nao capturada! Tentando continuar sem chave inicial...");
    } else if (s_AirPlay_DeriveAESKeySHA512ForScreen && s_AES_CTR_Init) {
        uint8_t key[16], iv[16];
        s_AirPlay_DeriveAESKeySHA512ForScreen(g_master_key, 16, connection_id, key, iv);
        memset(&g_alt_aes, 0, sizeof(g_alt_aes));
        if (s_AES_CTR_Init(&g_alt_aes, key, iv) == 0) {
            g_alt_aes_valid = 1;
            log_msg("RECV111", "Subchave AES-128-CTR e IV derivados para cid=%llu com sucesso!",
                    (unsigned long long)connection_id);
        }
        memset(key, 0, sizeof(key));
        memset(iv, 0, sizeof(iv));
    }

    int port = 6031;
    g_alt_listen_fd = bind_listener_stream111(port);
    if (g_alt_listen_fd < 0) {
        pthread_mutex_unlock(&g_crypto_lock);
        log_msg("RECV111", "Falha ao vincular porta TCP %d: %s", port, strerror(errno));
        return -1;
    }

    g_alt_thread_running = 1;
    if (pthread_create(&g_alt_thread, NULL, alt_receiver_thread, (void *)(intptr_t)g_alt_listen_fd) != 0) {
        close(g_alt_listen_fd);
        g_alt_listen_fd = -1;
        g_alt_thread_running = 0;
        pthread_mutex_unlock(&g_crypto_lock);
        log_msg("RECV111", "Falha ao criar alt_receiver_thread");
        return -1;
    }

    pthread_mutex_unlock(&g_crypto_lock);
    log_msg("RECV111", "Servidor TCP Stream 111 ouvindo na porta %d (cid=%llu)", port, (unsigned long long)connection_id);
    return port;
}

static void stop_alt_receiver(void) {
    pthread_mutex_lock(&g_crypto_lock);
    g_alt_thread_running = 0;
    if (g_alt_client_fd >= 0) {
        close(g_alt_client_fd);
        g_alt_client_fd = -1;
    }
    if (g_alt_listen_fd >= 0) {
        close(g_alt_listen_fd);
        g_alt_listen_fd = -1;
    }
    g_alt_aes_valid = 0;
    pthread_mutex_unlock(&g_crypto_lock);
}

/* =============================================================================
 * Inspecao de SETUP do CarPlay: Detecao de Stream 111
 * ============================================================================= */
static int stream_type(void *d) {
    if (!d || !s_CFDictionaryGetInt64) return -1;
    void *k = s_cf("type");
    if (!k) return -1;
    int32_t err = 0;
    int64_t v = s_CFDictionaryGetInt64(d, k, &err);
    if (s_CFRelease) s_CFRelease(k);
    return err ? -1 : (int)v;
}

static uint64_t stream_connection_id(void *d) {
    if (!d || !s_CFDictionaryGetInt64) return 0;
    void *k = s_cf("streamConnectionID");
    if (!k) return 0;
    int32_t err = 0;
    int64_t v = s_CFDictionaryGetInt64(d, k, &err);
    if (s_CFRelease) s_CFRelease(k);
    return err ? 0 : (uint64_t)v;
}

static void *get_streams(void *dict) {
    if (!dict || !s_CFDictionaryGetValue) return NULL;
    void *k = s_cf("streams");
    if (!k) return NULL;
    void *a = (void *)s_CFDictionaryGetValue(dict, k);
    if (s_CFRelease) s_CFRelease(k);
    return a;
}

static int contains_stream111(void *request, void **outDesc, int *outOtherCount) {
    void *a = get_streams(request);
    int found = 0, other = 0;
    if (outDesc) *outDesc = NULL;
    if (!a || !s_CFArrayGetCount) {
        if (outOtherCount) *outOtherCount = 0;
        return 0;
    }

    int64_t n = s_CFArrayGetCount(a);
    for (int64_t i = 0; i < n; i++) {
        void *sd = (void *)s_CFArrayGetValueAtIndex(a, i);
        if (stream_type(sd) == K_STREAM_ALT) {
            found = 1;
            if (outDesc && !*outDesc) *outDesc = sd;
        } else {
            other++;
        }
    }
    if (outOtherCount) *outOtherCount = other;
    return found;
}

static void append_alt_setup_response(void *response, void *requested, int data_port) {
    if (!response || !requested) return;

    void *kstreams = s_cf("streams");
    void *old = (kstreams && s_CFDictionaryGetValue) ? (void *)s_CFDictionaryGetValue(response, kstreams) : NULL;
    void *a = old ? array_clone(old) : NULL;
    if (!a && s_CFArrayCreateMutable) {
        a = s_CFArrayCreateMutable(NULL, 0, s_kCFLArrayCallBacks);
    }
    void *sd = dict_clone(requested);

    if (a && sd) {
        set_i64(sd, "dataPort", data_port);
        set_i64(sd, "streamID", K_STREAM_ALT);
        if (s_CFArrayAppendValue) {
            s_CFArrayAppendValue(a, sd);
        }
        if (s_CFDictionarySetValue && kstreams) {
            s_CFDictionarySetValue(response, kstreams, a);
        }
    }

    if (sd && s_CFRelease) s_CFRelease(sd);
    if (a && s_CFRelease) s_CFRelease(a);
    if (kstreams && s_CFRelease) s_CFRelease(kstreams);
}

/* =============================================================================
 * HOOK 1: AirPlayCopyServerInfo (libAirPlay.so)
 * Assinatura real AArch64: (void *session, void *properties, int32_t *outErr)
 * Injeta o Display 1 (Cluster AID 1280x480, Type 111) no GET /info do AirPlay
 * ============================================================================= */
typedef void *(*AirPlayCopyServerInfo_fn)(void *session, void *properties, int32_t *outErr);
static AirPlayCopyServerInfo_fn real_AirPlayCopyServerInfo = NULL;

void *AirPlayCopyServerInfo(void *session, void *properties, int32_t *outErr) {
    if (!real_AirPlayCopyServerInfo) {
        real_AirPlayCopyServerInfo = (AirPlayCopyServerInfo_fn)dlsym(RTLD_NEXT, "AirPlayCopyServerInfo");
    }

    int32_t err = 0;
    void *base = NULL;
    if (real_AirPlayCopyServerInfo) {
        base = real_AirPlayCopyServerInfo(session, properties, &err);
    }
    log_msg("SERVERINFO", "Stock AirPlayCopyServerInfo ret base=%p, err=%d", base, err);

    if (outErr) *outErr = err;
    if (!base || err != 0) return base;

    init_cf_symbols();

    /* 1. Clona o dicionario base do servidor para mutacao */
    void *info = dict_clone(base);
    if (!info) return base;

    /* 2. Habilita bit 26 em root features (AIRPLAY_FEATURE_BIT26) */
    void *k_features = s_cf("features");
    if (k_features) {
        int32_t get_err = 0;
        int64_t feat = s_CFDictionaryGetInt64 ? s_CFDictionaryGetInt64(info, k_features, &get_err) : 0;
        if (!get_err) {
            int64_t feat_new = (int64_t)((uint64_t)feat | AIRPLAY_FEATURE_BIT26);
            if (s_CFDictionarySetInt64) s_CFDictionarySetInt64(info, k_features, feat_new);
            log_msg("SERVERINFO", "Root features: 0x%llx -> 0x%llx (bit 26 ativado)",
                    (unsigned long long)feat, (unsigned long long)feat_new);
        }
        if (s_CFRelease) s_CFRelease(k_features);
    }

    /* 3. Define root enabledFeatures */
    int use_va = check_viewareas_enabled();
    set_reference_enabled_features(info, use_va);

    /* 4. Extrai e clona o array de displays */
    void *k_displays = s_cf("displays");
    void *stock_displays = (k_displays && s_CFDictionaryGetValue) ? (void *)s_CFDictionaryGetValue(base, k_displays) : NULL;
    if (stock_displays && s_CFArrayGetCount && s_CFArrayGetCount(stock_displays) > 0) {
        void *displays = array_clone(stock_displays);
        void *stock_disp0 = (void *)s_CFArrayGetValueAtIndex(stock_displays, 0);
        void *alt = dict_clone(stock_disp0);

        if (displays && alt) {
            remove_key(alt, "primaryInputDevice");
            remove_key(alt, "edid");
            remove_key(alt, "platformLayer");
            remove_key(alt, "avcc");
            remove_key(alt, "xOffset");
            remove_key(alt, "yOffset");
            remove_key(alt, "windowWidth");
            remove_key(alt, "windowHeight");

            set_i64(alt, "type", K_STREAM_ALT);
            set_i64(alt, "maxFPS", 30);
            set_i64(alt, "features", 0);
            set_i64(alt, "widthPixels", 1280);
            set_i64(alt, "heightPixels", 480);
            set_i64(alt, "widthPhysical", 320);
            set_i64(alt, "heightPhysical", 120);
            set_str(alt, "uuid", ALT_UUID_DEFAULT);
            set_str(alt, "initialURL", ALT_URL_DEFAULT);

            void *urls = NULL;
            if (s_CFArrayEnsureCreatedAndAppendCString) {
                s_CFArrayEnsureCreatedAndAppendCString(&urls, ALT_URL_DEFAULT, (size_t)-1);
                s_CFArrayEnsureCreatedAndAppendCString(&urls, ALT_URL_MAP, (size_t)-1);
                s_CFArrayEnsureCreatedAndAppendCString(&urls, ALT_URL_INSTRUCTIONCARD, (size_t)-1);
                void *k_urls = s_cf("altScreenSuggestUIURLs");
                if (k_urls && s_CFDictionarySetValue) {
                    s_CFDictionarySetValue(alt, k_urls, urls);
                    if (s_CFRelease) s_CFRelease(k_urls);
                }
                if (urls && s_CFRelease) s_CFRelease(urls);
            }

            if (use_va) {
                add_reference_viewarea(alt);
            }

            if (s_CFArrayAppendValue) {
                s_CFArrayAppendValue(displays, alt);
                if (s_CFDictionarySetValue) {
                    s_CFDictionarySetValue(info, k_displays, displays);
                }
                log_msg("SERVERINFO", ">>> DISPLAY 1 (Cluster AID 1280x480, Type 111, AltScreen%s) ANUNCIADO NO RTSP GET /info! <<<",
                        use_va ? "+viewAreas" : "");
            }
        }
        if (alt && s_CFRelease) s_CFRelease(alt);
        if (displays && s_CFRelease) s_CFRelease(displays);
    }
    if (k_displays && s_CFRelease) s_CFRelease(k_displays);

    if (s_CFPropertyListWriteToFilePath) {
        s_CFPropertyListWriteToFilePath(info, "/data/lge/smartlink/carplay/server_info_v62.plist", 0);
    }

    if (s_CFRelease) s_CFRelease(base);
    if (outErr) *outErr = 0;
    return info;
}

/* =============================================================================
 * HOOK 2: AirPlayReceiverSessionSetup (libAirPlay.so)
 * Trata o SETUP do Stream 111 com servidor TCP local (Porta 6031)
 * ============================================================================= */
typedef int32_t (*AirPlayReceiverSessionSetup_fn)(void *inSession, void *inRequest, void **outResponse);
static AirPlayReceiverSessionSetup_fn real_AirPlayReceiverSessionSetup = NULL;

int32_t AirPlayReceiverSessionSetup(void *inSession, void *inRequest, void **outResponse) {
    if (!real_AirPlayReceiverSessionSetup) {
        real_AirPlayReceiverSessionSetup = (AirPlayReceiverSessionSetup_fn)dlsym(RTLD_NEXT, "AirPlayReceiverSessionSetup");
    }

    init_cf_symbols();
    init_crypto_symbols();

    void *alt_desc = NULL;
    int other_count = 0;
    int has_alt = inRequest ? contains_stream111(inRequest, &alt_desc, &other_count) : 0;

    log_msg("SETUP", "AirPlayReceiverSessionSetup: has_stream111=%d, other_streams=%d, session=%p",
            has_alt, other_count, inSession);

    int32_t ret = 0;
    if (real_AirPlayReceiverSessionSetup) {
        ret = real_AirPlayReceiverSessionSetup(inSession, inRequest, outResponse);
    }

    log_msg("SETUP", "Stock AirPlayReceiverSessionSetup ret=%d, outResponse=%p",
            ret, (outResponse ? *outResponse : NULL));

    if (ret == 0 && outResponse && *outResponse) {
        int use_va = check_viewareas_enabled();
        set_reference_enabled_features(*outResponse, use_va);

        if (has_alt && alt_desc) {
            uint64_t cid = stream_connection_id(alt_desc);
            log_msg("SETUP", ">>> SOLICITACAO DE STREAM 111 DETECTADA! cid=%llu <<<", (unsigned long long)cid);

            int port = start_alt_receiver(cid);
            if (port > 0) {
                append_alt_setup_response(*outResponse, alt_desc, port);
                log_msg("SETUP", ">>> STREAM 111 NEGOCIADO COM SUCESSO! dataPort=%d, streamID=111 <<<", port);
            } else {
                log_msg("SETUP", "Falha ao iniciar receptor TCP do Stream 111!");
            }
        }
    }

    return ret;
}

/* =============================================================================
 * HOOK 3: AirPlayReceiverSessionTearDown (libAirPlay.so)
 * ============================================================================= */
typedef void (*AirPlayReceiverSessionTearDown_fn)(void *inSession, void *inRequest, int32_t inReason, uint8_t *outDone);
static AirPlayReceiverSessionTearDown_fn real_AirPlayReceiverSessionTearDown = NULL;

void AirPlayReceiverSessionTearDown(void *inSession, void *inRequest, int32_t inReason, uint8_t *outDone) {
    if (!real_AirPlayReceiverSessionTearDown) {
        real_AirPlayReceiverSessionTearDown = (AirPlayReceiverSessionTearDown_fn)dlsym(RTLD_NEXT, "AirPlayReceiverSessionTearDown");
    }

    log_msg("TEARDOWN", "AirPlayReceiverSessionTearDown: session=%p, reason=%d", inSession, inReason);
    stop_alt_receiver();

    if (real_AirPlayReceiverSessionTearDown) {
        real_AirPlayReceiverSessionTearDown(inSession, inRequest, inReason, outDone);
    }
}

/* =============================================================================
 * HOOK 4: ScreenStreamCreate, ScreenStreamStop & ScreenStreamSetWidthHeight (libAirPlaySupport.so)
 * Garante que Display 0 (Central Multimidia 784x468) permaneca 100% nativo para o Cinemo
 * ============================================================================= */
typedef int32_t (*ScreenStreamCreate_fn)(void **outStream);
static ScreenStreamCreate_fn real_ScreenStreamCreate = NULL;

int32_t ScreenStreamCreate(void **outStream) {
    if (!real_ScreenStreamCreate) {
        real_ScreenStreamCreate = (ScreenStreamCreate_fn)dlsym(RTLD_NEXT, "ScreenStreamCreate");
    }
    int32_t ret = 0;
    if (real_ScreenStreamCreate) {
        ret = real_ScreenStreamCreate(outStream);
    }
    log_msg("STREAM", "ScreenStreamCreate ret=%d (streamRef=%p)", ret, (outStream ? *outStream : NULL));
    return ret;
}

typedef int32_t (*ScreenStreamStop_fn)(void *inStream);
static ScreenStreamStop_fn real_ScreenStreamStop = NULL;

int32_t ScreenStreamStop(void *inStream) {
    if (!real_ScreenStreamStop) {
        real_ScreenStreamStop = (ScreenStreamStop_fn)dlsym(RTLD_NEXT, "ScreenStreamStop");
    }
    log_msg("STREAM", "ScreenStreamStop: streamRef=%p", inStream);
    if (real_ScreenStreamStop) {
        return real_ScreenStreamStop(inStream);
    }
    return 0;
}

typedef void (*ScreenStreamSetWidthHeight_fn)(void *inStream, uint32_t inWidth, uint32_t inHeight);
static ScreenStreamSetWidthHeight_fn real_ScreenStreamSetWidthHeight = NULL;

void ScreenStreamSetWidthHeight(void *inStream, uint32_t inWidth, uint32_t inHeight) {
    if (!real_ScreenStreamSetWidthHeight) {
        real_ScreenStreamSetWidthHeight = (ScreenStreamSetWidthHeight_fn)dlsym(RTLD_NEXT, "ScreenStreamSetWidthHeight");
    }
    log_msg("STREAM", "ScreenStreamSetWidthHeight: streamRef=%p, %ux%u (Display 0 Cinemo mantido nativo)",
            inStream, inWidth, inHeight);
    if (real_ScreenStreamSetWidthHeight) {
        real_ScreenStreamSetWidthHeight(inStream, inWidth, inHeight);
    }
}

/* =============================================================================
 * Ciclo de Vida da Biblioteca
 * ============================================================================= */
__attribute__((constructor))
static void altscreen_init(void) {
    log_msg("INIT", "=================================================");
    log_msg("INIT", "altscreen_handler.so v6.2 (AltScreen Gen-2 Cluster Streamer) ativo no PID: %d", getpid());
    log_msg("INIT", "Display 0: Cinemo 100%% nativo | Display 1: Type 111 (1280x480) -> RTP [ff14::2:4]:42800 (PID 17, eth0.5)");
    log_msg("INIT", "=================================================");
    init_cf_symbols();
    init_crypto_symbols();
    init_cluster_network();
}

__attribute__((destructor))
static void altscreen_cleanup(void) {
    log_msg("CLEANUP", "altscreen_handler.so finalizado (PID: %d)", getpid());
    stop_alt_receiver();
    if (g_rtp_sock >= 0) {
        close(g_rtp_sock);
        g_rtp_sock = -1;
    }
    if (g_kmsg_fd >= 0) {
        close(g_kmsg_fd);
        g_kmsg_fd = -1;
    }
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}
