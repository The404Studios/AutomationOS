/*
 * kernel/include/strerror.h - Error message conversion
 */

#ifndef STRERROR_H
#define STRERROR_H

const char* strerror(int err);
const char* strerrorname(int err);

#endif /* STRERROR_H */
