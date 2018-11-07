#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char *strreplace(char const *const original, char const *const pattern, char const *const replacement)
{
    size_t const replen = strlen(replacement);
    size_t const patlen = strlen(pattern);
    size_t const orilen = strlen(original);
    size_t patcnt = 0;
    const char *oriptr;
    const char *patloc;
    // find how many times the pattern occurs in the original string
    for (oriptr = original; (patloc = strstr(oriptr, pattern)); oriptr = patloc + patlen)
    {
        patcnt++;
    }
    {
        // allocate memory for the new string
        size_t const retlen = orilen + patcnt * (replen - patlen);
        char *const returned = (char *)malloc(sizeof(char) * (retlen + 1));
        if (returned != NULL)
        {
            // copy the original string,
            // replacing all the instances of the pattern
            char *retptr = returned;
            for (oriptr = original; (patloc = strstr(oriptr, pattern)); oriptr = patloc + patlen)
            {
                size_t const skplen = patloc - oriptr;
                // copy the section until the occurence of the pattern
                strncpy(retptr, oriptr, skplen);
                retptr += skplen;
                // copy the replacement
                strncpy(retptr, replacement, replen);
                retptr += replen;
            }
            // copy the rest of the string.
            strcpy(retptr, oriptr);
        }
        return returned;
    }
}

char *find_target_str(char const *const original, char const *const pattern1, char const *const pattern2)
{
    const char *p1_locate = strstr(original, pattern1);
    const char *p2_locate = strstr(p1_locate, pattern2);
    size_t const p1_len = strlen(pattern1);
    // find the target string len, drop paterns str
    size_t const pattern_len = p2_locate - p1_locate - p1_len;
    // allocate memory for the target string
    char *const target_name = (char *)malloc(sizeof(char) * (pattern_len));
    //copy out target str between pattern1 and pattern2
    strncpy(target_name, p1_locate + p1_len, pattern_len);
    return target_name;
}

char *find_target_str_with_pattern(char const *const original, char const *const pattern1, char const *const pattern2)
{
    const char *p1_locate = strstr(original, pattern1);
    const char *p2_locate = strstr(p1_locate, pattern2);
    size_t const p2_len = strlen(pattern2);
    // find the target string len, drop paterns str
    size_t const pattern_len = p2_locate - p1_locate + p2_len;
    // allocate memory for the target string
    char *const target_name = (char *)malloc(sizeof(char) * (pattern_len));
    //copy out target str between pattern1 and pattern2
    strncpy(target_name, p1_locate, pattern_len);
    return target_name;
}

int check_pattern_in_str(char const *const original, char const *const pattern)
{
    if (strstr(original, pattern))
        return 0;
    else
        return -1;
}

enum STRATUM_MSG_TYPE check_stratum_msg_type(char const *const original)
{
    int stm_msg_type = 0;
    if (check_pattern_in_str(original, STM_AUTH_KEY) == 0)
        stm_msg_type += STM_AUTH;
    if (check_pattern_in_str(original, STM_SUBSCRIBE_KEY) == 0)
        stm_msg_type += STM_SUBSCRIBE;
    if (check_pattern_in_str(original, STM_SUBMIT_KEY) == 0)
        stm_msg_type += STM_SUBMIT;
    if (check_pattern_in_str(original, STM_SET_DIFFICULT_KEY) == 0)
        stm_msg_type += STM_SET_DIFFICULT;
    if (check_pattern_in_str(original, STM_NOTIFY_KEY) == 0)
        stm_msg_type += STM_NOTIFY;
    return stm_msg_type;
}