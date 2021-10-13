/*
Copyright (c) 2019-2020 Roger Light <roger@atchoo.org>

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

#ifndef ALIAS_dimq_H
#define ALIAS_dimq_H

#include "dimq_internal.h"

int alias__add(struct dimq *dimq, const char *topic, uint16_t alias);
int alias__find(struct dimq *dimq, char **topic, uint16_t alias);
void alias__free_all(struct dimq *dimq);

#endif