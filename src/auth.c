#include "auth.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

bool auth_verify(const char *token, user_info_t *out){
    if(!token||!out) return false;
    const char *user=NULL, *role=NULL;
    if(strncmp(token,"valid_",6)==0&&token[6]){user=token+6;role="user";}
    else if(strncmp(token,"admin_",6)==0&&token[6]){user=token+6;role="admin";}
    else{LOGD("auth rejected: %.32s",token);return false;}
    memset(out,0,sizeof(*out));
    snprintf(out->session_token,sizeof(out->session_token),"%s",token);
    snprintf(out->username,sizeof(out->username),"%s",user);
    snprintf(out->role,sizeof(out->role),"%s",role);
    LOGD("auth ok user=%s role=%s",out->username,out->role);
    return true;
}