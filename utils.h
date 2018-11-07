#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

#define STM_SUBSCRIBE_KEY "mining.subscribe"
#define STM_AUTH_KEY "mining.authorize"
#define STM_SET_DIFFICULT_KEY "mining.set_difficulty"
#define STM_NOTIFY_KEY "mining.notify"
#define STM_SUBMIT_KEY "mining.submit"

enum STRATUM_MSG_TYPE
{
    STM_ACK = 0,
    STM_SUBSCRIBE = 1,
    STM_AUTH = 2,
    STM_SET_DIFFICULT = 4,
    STM_NOTIFY = 8,
    STM_SUBMIT = 16,
    //above is base type
    STM_INIT_SUBSCRIBE = STM_SET_DIFFICULT + STM_NOTIFY
};

char *strreplace(char const *const original, char const *const pattern, char const *const replacement);

char *find_target_str(char const *const original, char const *const pattern1, char const *const pattern2);
char *find_target_str_with_pattern(char const *const original, char const *const pattern1, char const *const pattern2);
int check_pattern_in_str(char const *const original, char const *const pattern);
enum STRATUM_MSG_TYPE check_stratum_msg_type(char const *const original);

#endif