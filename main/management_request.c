#include "management_request.h"
#include <math.h>
#include <string.h>
cJSON *management_json_parse(const char *data,size_t used)
{
    if (!data || !used || used>1024 || data[used]) return NULL;
    unsigned depth=0; bool quoted=false,escaped=false;
    for (size_t i=0;i<used;i++) {
        char c=data[i]; if (!c) return NULL;
        if (quoted) {
            if (escaped) { escaped=false; continue; }
            if (c=='\\') { if (!strncmp(data+i,"\\u0000",6)) return NULL; escaped=true; }
            else if (c=='"') quoted=false;
        } else if (c=='"') quoted=true;
        else if (c=='{' || c=='[') { if (++depth>2) return NULL; }
        else if (c=='}' || c==']') { if (!depth) return NULL; --depth; }
    }
    if (quoted || depth) return NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(data,used+1,NULL,true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    for (cJSON *a=root->child;a;a=a->next) for (cJSON *b=a->next;b;b=b->next)
        if (!strcmp(a->string,b->string)) { cJSON_Delete(root); return NULL; }
    return root;
}
bool management_number(const cJSON *v,unsigned low,unsigned high)
{
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) &&
        v->valuedouble>=low && v->valuedouble<=high && floor(v->valuedouble)==v->valuedouble;
}
static bool copy_string(char *out,size_t capacity,const cJSON *v,bool keep_empty)
{
    if (!cJSON_IsString(v) || strlen(v->valuestring)>=capacity) return false;
    if (!keep_empty || v->valuestring[0]) strcpy(out,v->valuestring);
    return true;
}
bool management_settings_patch(const cJSON *root,gateway_settings_t *next)
{
    bool valid=cJSON_IsObject(root) && root->child;
    if (!valid) return false;
    for (cJSON *v=root->child;v;v=v->next) {
#define STRING(field,keep) if (!strcmp(v->string,#field)) { valid=copy_string(next->field,sizeof(next->field),v,keep); }
        STRING(sta_ssid,false)
        else STRING(sta_password,true)
        else STRING(ap_ssid,false)
        else STRING(ap_password,true)
        else STRING(socks_ipv4,false)
        else STRING(dns_ipv4,false)
        else STRING(admin_password,true)
        else if (!strcmp(v->string,"socks_port")) { valid=management_number(v,1,65535); if (valid) next->socks_port=v->valueint; }
        else if (!strcmp(v->string,"max_clients")) { valid=management_number(v,1,2); if (valid) next->max_clients=v->valueint; }
        else if (!strcmp(v->string,"ap_subnet")) { valid=management_number(v,0,255); if (valid) next->ap_subnet=v->valueint; }
        else if (!strcmp(v->string,"sta_open")) {
            valid=cJSON_IsBool(v); /* Applied after all fields, regardless of key order. */
        } else valid=false;
#undef STRING
        if (!valid) break;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"sta_open"))) next->sta_password[0]=0;
    return valid && gateway_settings_valid(next);
}
