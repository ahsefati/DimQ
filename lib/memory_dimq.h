/*
Copyright (c) 2010-2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#ifndef MEMORY_dimq_H
#define MEMORY_dimq_H

#include <stdio.h>
#include <sys/types.h>

#if defined(WITH_MEMORY_TRACKING) && defined(WITH_BROKER)
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__GLIBC__)
#    define REAL_WITH_MEMORY_TRACKING
#  endif
#endif

void *dimq__calloc(size_t nmemb, size_t size);
void dimq__free(void *mem);
void *dimq__malloc(size_t size);
#ifdef REAL_WITH_MEMORY_TRACKING
unsigned long dimq__memory_used(void);
unsigned long dimq__max_memory_used(void);
#endif
void *dimq__realloc(void *ptr, size_t size);
char *dimq__strdup(const char *s);

#ifdef WITH_BROKER
void memory__set_limit(size_t lim);
#endif

#endif
