#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "api_idempotency.h"
#include "nvs.h"
#include "freertos/semphr.h"

typedef struct { char key[8]; unsigned char *data; size_t len; int used; } blob_t;
static blob_t blobs[GATEWAY_IDEMPOTENCY_MAX_RECORDS];

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) { assert(strcmp(name,"api_idem")==0); assert(mode==NVS_READWRITE); *out=1; return ESP_OK; }
static blob_t *find_blob(const char *key, int create) {
    for (size_t i=0;i<GATEWAY_IDEMPOTENCY_MAX_RECORDS;i++) if (blobs[i].used && strcmp(blobs[i].key,key)==0) return &blobs[i];
    if (!create) return NULL;
    for (size_t i=0;i<GATEWAY_IDEMPOTENCY_MAX_RECORDS;i++) if (!blobs[i].used) { blobs[i].used=1; strncpy(blobs[i].key,key,sizeof(blobs[i].key)-1); return &blobs[i]; }
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *len) { (void)h; blob_t *b=find_blob(key,0); if(!b) return ESP_ERR_NVS_NOT_FOUND; if(!len) return ESP_ERR_INVALID_ARG; if(!out){*len=b->len;return ESP_OK;} if(*len<b->len)return ESP_ERR_INVALID_SIZE; memcpy(out,b->data,b->len);*len=b->len;return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t len) { (void)h; blob_t *b=find_blob(key,1); if(!b)return ESP_ERR_NO_MEM; unsigned char *p=malloc(len); if(!p)return ESP_ERR_NO_MEM; memcpy(p,data,len); free(b->data);b->data=p;b->len=len;return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) { (void)h; blob_t *b=find_blob(key,0); if(!b)return ESP_ERR_NVS_NOT_FOUND; free(b->data);memset(b,0,sizeof(*b));return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
/* Unused host-stub declarations. */
esp_err_t nvs_open_from_partition(const char*a,const char*b,int c,nvs_handle_t*d){(void)a;(void)b;(void)c;(void)d;return ESP_FAIL;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char*k,uint32_t*out){(void)h;(void)k;(void)out;return ESP_FAIL;}
esp_err_t nvs_set_u32(nvs_handle_t h,const char*k,uint32_t v){(void)h;(void)k;(void)v;return ESP_FAIL;}

esp_err_t gateway_security_sha256(const void *data,size_t len,uint8_t out[GATEWAY_SHA256_LEN]) {
    const unsigned char *p=data; uint32_t h=2166136261u;
    for(size_t i=0;i<len;i++){h^=p[i];h*=16777619u;}
    for(size_t i=0;i<GATEWAY_SHA256_LEN;i++){h^=(uint32_t)i*0x9e3779b9u;h*=16777619u;out[i]=(uint8_t)(h>>(8*(i&3)));}
    return ESP_OK;
}
void gateway_security_wipe(void *data,size_t len){volatile unsigned char *p=data;while(len--)*p++=0;}

int main(void) {
    assert(gateway_idempotency_init()==ESP_OK);
    uint8_t a[GATEWAY_SHA256_LEN], b[GATEWAY_SHA256_LEN];
    assert(gateway_idempotency_sms_fingerprint("+49123","hello",true,a)==ESP_OK);
    assert(gateway_idempotency_sms_fingerprint("+49123","different",true,b)==ESP_OK);
    gateway_idempotency_result_t r; uint32_t id;
    assert(gateway_idempotency_claim("request-0001",a,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS && id==0);
    assert(gateway_idempotency_claim("request-0001",a,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_REPLAY && id==0);
    assert(gateway_idempotency_claim("request-0001",b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_CONFLICT);
    assert(gateway_idempotency_finalize("request-0001",a,42)==ESP_OK);
    assert(gateway_idempotency_lookup("request-0001",a,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_REPLAY && id==42);
    assert(gateway_idempotency_release_pending("request-0001",a)==ESP_ERR_INVALID_STATE);
    assert(gateway_idempotency_claim("request-0002",b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS);
    assert(gateway_idempotency_release_pending("request-0002",b)==ESP_OK);
    assert(gateway_idempotency_claim("request-0002",b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS);
    char key[32];
    for (unsigned i = 3; i <= 32; ++i) {
        (void)snprintf(key, sizeof(key), "request-%04u", i);
        assert(gateway_idempotency_claim(key,b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS);
    }
    assert(gateway_idempotency_claim("request-0033",b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS); /* evicts only finalized record */
    assert(gateway_idempotency_claim("request-0034",b,&r,&id)==ESP_ERR_NO_MEM); /* pending claims are never evicted */
    assert(gateway_idempotency_release_pending("request-0002",b)==ESP_OK);
    assert(gateway_idempotency_claim("request-0034",b,&r,&id)==ESP_OK && r==GATEWAY_IDEMPOTENCY_MISS);
    gateway_idempotency_diagnostics_t diag;
    assert(gateway_idempotency_get_diagnostics(&diag)==ESP_OK);
    assert(diag.pending_records > 0 && diag.free_records == 0);
    uint32_t cleared = 0;
    assert(gateway_idempotency_clear_pending(&cleared)==ESP_OK && cleared == diag.pending_records);
    assert(gateway_idempotency_get_diagnostics(&diag)==ESP_OK && diag.pending_records == 0 && diag.free_records == cleared);
    return 0;
}
