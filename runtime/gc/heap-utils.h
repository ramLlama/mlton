/* Copyright (C) 2014 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file heap-utils.h
 *
 * @author Ram Raghunathan
 *
 * This file provides many useful utility functions for the HeapManagement
 * namespace.
 */

#ifndef HEAP_UTILS_H_
#define HEAP_UTILS_H_

/**
 * This function prints the debug message if 'DEBUG_HEAP_MANAGEMENT' or
 * 's->controls->heapManagementmessages' is set
 *
 * @param s The GC_state to use
 * @param format The message format as per 'printf()'
 * @param ... The format arguments as per 'printf()'
 */
void HeapManagement_debugMessage (GC_state s, const char* format, ...)
    __attribute__((format (printf, 2, 3)));

#endif /* HEAP_UTILS_H_ */
