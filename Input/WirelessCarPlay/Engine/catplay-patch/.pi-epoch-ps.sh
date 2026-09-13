#!/usr/bin/env bash
ps -eo pid,stat,etimes,cmd | grep -E 'cargo|rustc|cc1|make' | grep -v grep || true
