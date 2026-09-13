#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Orange Pi Zero 2W 极简 .config 变换器
用法: minimize_config.py <.config 路径> <spec 路径>
读取 spec:
    !OFF <前缀>    -> 把所有 CONFIG_<前缀>* 的 =y/=m 注释掉
    NAME=value     -> 精确设置(优先级最高, 支持一行多个)
    #  开头是注释
未知符号会被写进 .config, 随后 make olddefconfig 自动丢弃。
"""
import re, sys

KEY = re.compile(r'^[A-Za-z0-9_]+$')

def main():
    cfg_path, spec_path = sys.argv[1], sys.argv[2]
    prefixes, sets = [], {}
    with open(spec_path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('!OFF'):
                parts = line.split(None, 1)
                if len(parts) == 2 and parts[1].strip():
                    prefixes.append(parts[1].strip())
                else:
                    print('!! 非法 !OFF 行: %r' % line)
                continue
            toks = [line] if re.match(r'^[A-Za-z0-9_]+="', line) else line.split()
            for tok in toks:
                if '=' not in tok:
                    print('!! 忽略(缺少 =): %r' % line)
                    continue
                name, val = tok.split('=', 1)
                if not KEY.match(name):
                    print('!! 忽略(非法符号名): %r' % tok)
                    continue
                if name.startswith('CONFIG_'):
                    name = name[len('CONFIG_'):]
                sets[name] = val
    prefixes = sorted(set(prefixes))

    out, seen, off_cnt, set_cnt = [], set(), 0, 0
    with open(cfg_path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n')
            m = re.match(r'^CONFIG_([A-Za-z0-9_]+)=(.*)$', line)
            if m:
                name, val = m.group(1), m.group(2)
                seen.add(name)
                if name in sets:
                    nv = sets[name]
                    set_cnt += 1
                    out.append('# CONFIG_%s is not set' % name if nv == 'n'
                               else 'CONFIG_%s=%s' % (name, nv))
                    continue
                if val in ('y', 'm') and any(name.startswith(p) for p in prefixes):
                    off_cnt += 1
                    out.append('# CONFIG_%s is not set' % name)
                    continue
                out.append(line)
                continue
            m = re.match(r'^#\s*CONFIG_([A-Za-z0-9_]+) is not set\s*$', line)
            if m:
                name = m.group(1)
                if name in sets and sets[name] != 'n':
                    out.append('CONFIG_%s=%s' % (name, sets[name]))
                    seen.add(name)
                    set_cnt += 1
                    continue
                out.append(line)
                continue
            out.append(line)

    with open(cfg_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out).rstrip('\n') + '\n')
        f.write('\n# ==== 以下由 zero2w-minimal.spec 追加 ==== \n')
        for name, val in sets.items():
            if val == 'n' or name in seen:
                continue
            f.write('CONFIG_%s=%s\n' % (name, val))

    print('spec: %d 个前缀批量关闭, %d 个精确设置; 实际关闭 %d 项, 改写 %d 项'
          % (len(prefixes), len(sets), off_cnt, set_cnt))

main()
