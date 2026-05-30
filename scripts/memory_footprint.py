#!/usr/bin/env python3
"""
Memory Footprint Calculator for micrOSAL
Analyzes pool configurations and calculates RAM/FLASH usage per backend.
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


# Backend pool configurations with default sizes
BACKEND_POOLS = {
    'FREERTOS': {
        'pools': [
            ('OSAL_FREERTOS_MAX_MUTEXES', 16, 80),
            ('OSAL_FREERTOS_MAX_SEMAPHORES', 16, 80),
            ('OSAL_FREERTOS_MAX_TIMERS', 8, 88),
            ('OSAL_FREERTOS_MAX_EVENT_GROUPS', 8, 64),
            ('OSAL_FREERTOS_MAX_STREAM_BUFFERS', 8, 64),
            ('OSAL_FREERTOS_MAX_MESSAGE_BUFFERS', 8, 64),
            ('OSAL_FREERTOS_MAX_THREADS', 8, 96),
        ],
        'per_object_overhead': 12,
    },
    'ZEPHYR': {
        'pools': [
            ('OSAL_ZEPHYR_MAX_MUTEXES', 16, 48),
            ('OSAL_ZEPHYR_MAX_SEMAPHORES', 16, 32),
            ('OSAL_ZEPHYR_MAX_TIMERS', 8, 64),
            ('OSAL_ZEPHYR_MAX_EVENT_GROUPS', 8, 56),
            ('OSAL_ZEPHYR_MAX_THREADS', 8, 128),
            ('OSAL_ZEPHYR_MAX_WORK_QUEUES', 4, 96),
        ],
        'per_object_overhead': 12,
    },
    'THREADX': {
        'pools': [
            ('OSAL_THREADX_MAX_MUTEXES', 16, 64),
            ('OSAL_THREADX_MAX_SEMAPHORES', 16, 56),
            ('OSAL_THREADX_MAX_TIMERS', 8, 80),
            ('OSAL_THREADX_MAX_EVENT_GROUPS', 8, 64),
            ('OSAL_THREADX_MAX_THREADS', 8, 112),
        ],
        'per_object_overhead': 12,
    },
    'EMULATED': {
        'pools': [
            ('OSAL_EMULATED_CONDVAR_POOL_SIZE', 16, 200),
            ('OSAL_EMULATED_MPOOL_POOL_SIZE', 8, 128),
            ('OSAL_EMULATED_WQ_POOL_SIZE', 4, 512),
            ('OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE', 8, 96),
        ],
        'per_object_overhead': 0,
    },
}


def parse_config_file(file_path: Path) -> Dict[str, int]:
    """Parse configuration file or source to extract pool size macros."""
    config = {}
    if not file_path.exists():
        return config

    content = file_path.read_text()
    # Match: #define MACRO_NAME value
    pattern = r'#define\s+(OSAL_\w+_MAX_\w+|\OSAL_\w+_POOL_SIZE)\s+(\d+)'
    for match in re.finditer(pattern, content):
        macro_name = match.group(1)
        value = int(match.group(2))
        config[macro_name] = value

    return config


def calculate_backend_footprint(backend: str, overrides: Dict[str, int] = None) -> Tuple[int, List[str]]:
    """Calculate RAM footprint for a backend configuration."""
    if backend not in BACKEND_POOLS:
        return 0, [f"Unknown backend: {backend}"]

    config = BACKEND_POOLS[backend]
    overrides = overrides or {}

    total_bytes = 0
    details = []
    details.append(f"\n{'='*70}")
    details.append(f"{backend} Backend Memory Footprint")
    details.append(f"{'='*70}")
    details.append(f"\n{'Pool Type':<40} {'Count':>8} {'Per Item':>10} {'Total':>10}")
    details.append(f"{'-'*70}")

    for macro_name, default_count, size_per_item in config['pools']:
        count = overrides.get(macro_name, default_count)
        pool_total = count * size_per_item
        total_bytes += pool_total

        pool_name = macro_name.replace('OSAL_', '').replace(backend + '_', '')
        details.append(f"{pool_name:<40} {count:>8} {size_per_item:>9}B {pool_total:>9}B")

    details.append(f"{'-'*70}")
    details.append(f"{'Backend Pools Total':<40} {total_bytes:>29}B")

    overhead = config['per_object_overhead']
    if overhead > 0:
        details.append(f"\n{'Per-Object Overhead (handle + valid)':<40} {overhead:>29}B")
        details.append(f"{'(paid per instantiated primitive)':<40}")

    details.append(f"\n{'TOTAL RAM (static pools)':<40} {total_bytes:>29}B")
    details.append(f"{'':<40} {total_bytes/1024:>28.2f} KiB")

    return total_bytes, details


def calculate_queue_footprint(element_size: int, capacity: int) -> Tuple[int, List[str]]:
    """Calculate footprint for queue<T, N>."""
    storage = element_size * capacity
    handle = 12  # handle_ + valid_ + alignment
    total = storage + handle

    details = []
    details.append(f"\nqueue<T, {capacity}> where sizeof(T) = {element_size}B:")
    details.append(f"  - Storage: {capacity} × {element_size}B = {storage}B")
    details.append(f"  - Handle overhead: {handle}B")
    details.append(f"  - Total per instance: {total}B")

    return total, details


def calculate_ring_buffer_footprint(element_size: int, capacity: int) -> Tuple[int, List[str]]:
    """Calculate footprint for ring_buffer<T, N>."""
    # Ring buffer has N+1 capacity internally
    storage = element_size * (capacity + 1)
    overhead = 16  # two atomic<size_t> for head/tail
    total = storage + overhead

    details = []
    details.append(f"\nring_buffer<T, {capacity}> where sizeof(T) = {element_size}B:")
    details.append(f"  - Storage: ({capacity}+1) × {element_size}B = {storage}B")
    details.append(f"  - Head/tail atomics: {overhead}B")
    details.append(f"  - Total per instance: {total}B")

    return total, details


def main():
    parser = argparse.ArgumentParser(
        description='Calculate micrOSAL memory footprint',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s --backend FREERTOS
  %(prog)s --backend ZEPHYR --config my_config.h
  %(prog)s --queue 4 32  # queue<uint32_t, 32>
  %(prog)s --ring 2 64   # ring_buffer<uint16_t, 64>
        '''
    )

    parser.add_argument('--backend', choices=['FREERTOS', 'ZEPHYR', 'THREADX', 'EMULATED'],
                        help='Backend to analyze')
    parser.add_argument('--config', type=Path,
                        help='Configuration file with pool size overrides')
    parser.add_argument('--queue', nargs=2, type=int, metavar=('ELEM_SIZE', 'CAPACITY'),
                        help='Calculate queue<T,N> footprint')
    parser.add_argument('--ring', nargs=2, type=int, metavar=('ELEM_SIZE', 'CAPACITY'),
                        help='Calculate ring_buffer<T,N> footprint')
    parser.add_argument('--all-backends', action='store_true',
                        help='Show all backends')

    args = parser.parse_args()

    if not any([args.backend, args.queue, args.ring, args.all_backends]):
        parser.print_help()
        return 1

    # Parse config overrides
    overrides = {}
    if args.config:
        overrides = parse_config_file(args.config)
        if overrides:
            print(f"\nLoaded {len(overrides)} configuration overrides from {args.config}")

    # Backend analysis
    if args.backend or args.all_backends:
        backends = list(BACKEND_POOLS.keys()) if args.all_backends else [args.backend]

        total_all = 0
        for backend in backends:
            total, details = calculate_backend_footprint(backend, overrides)
            for line in details:
                print(line)
            total_all += total

        if args.all_backends:
            print(f"\n{'='*70}")
            print(f"{'ALL BACKENDS TOTAL':<40} {total_all:>29}B")
            print(f"{'':<40} {total_all/1024:>28.2f} KiB")

    # Queue analysis
    if args.queue:
        elem_size, capacity = args.queue
        total, details = calculate_queue_footprint(elem_size, capacity)
        for line in details:
            print(line)

    # Ring buffer analysis
    if args.ring:
        elem_size, capacity = args.ring
        total, details = calculate_ring_buffer_footprint(elem_size, capacity)
        for line in details:
            print(line)

    print()  # Final newline
    return 0


if __name__ == '__main__':
    sys.exit(main())
