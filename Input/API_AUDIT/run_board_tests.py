"""Run hardware-independent input regression binaries on the USB-connected ARM64 board.

Password supplied interactively through stdin, never stored in the repository.
Uses a fresh /tmp directory, does not replace or restart production services.
"""
import argparse
import io
import json
import os
from pathlib import Path
import shlex
import sys
import tarfile
import time
import paramiko

# Windows consoles may default to GBK; split UTF-8 SSH chunks can contain a
# replacement character. Printing diagnostics must not abort an ARM test run.
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.stderr.reconfigure(encoding='utf-8', errors='replace')

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description='Build and run isolated native Input API regressions over SSH; password comes from stdin, never stored.')
parser.add_argument('--host', default='192.168.77.2')
args = parser.parse_args()
password = sys.stdin.readline().rstrip('\r\n')
client = paramiko.SSHClient()
client.load_system_host_keys()
client.connect(args.host, username='root', password=password,
               look_for_keys=False, allow_agent=False, timeout=5, auth_timeout=5)
del password

def command(cmd, limit=180):
    _, out, _ = client.exec_command(cmd, timeout=limit)
    channel = out.channel
    channel.set_combine_stderr(True)
    output = bytearray()
    deadline = time.monotonic() + limit + 5
    while True:
        while channel.recv_ready():
            block = channel.recv(8192)
            output.extend(block)
            print(block.decode('utf-8', errors='replace'), end='', flush=True)
        if channel.exit_status_ready() and not channel.recv_ready():
            break
        if time.monotonic() > deadline:
            channel.close()
            raise TimeoutError(cmd)
        time.sleep(.1)
    return channel.recv_exit_status(), output.decode('utf-8', errors='replace')

status, folder = command('mktemp -d /tmp/zero2w-input-api-XXXXXXXX', 5)
folder = folder.strip()
if status or not folder.startswith('/tmp/zero2w-input-api-') or '/' in folder[len('/tmp/'):]:
    raise RuntimeError('Invalid isolated test directory')

archive = io.BytesIO()
paths = [
    'Core/MainMenu/include', 'Core/MainMenu/src/session_core.cpp',
    'Core/Convert/include', 'Core/Convert/src/real_media_store.cpp',
    'Core/Web/include', 'Core/Web/src/display_config.cpp',
    'Input/WirelessCarPlay/include', 'Input/WirelessCarPlay/src/catplay_media_client.cpp',
    'Input/WirelessCarPlay/tests/input_api_tests.cpp',
    'Input/WirelessCarPlay/tests/media_ext_tests.cpp',
    'Input/WirelessCarLifePlus/include', 'Input/WirelessCarLifePlus/src',
    'Input/WirelessCarLifePlus/tests/input_api_tests.cpp',
]
with tarfile.open(fileobj=archive, mode='w:gz') as tar:
    for name in paths:
        tar.add(root / name, arcname=name)
archive.seek(0)
with client.open_sftp() as sftp:
    sftp.putfo(archive, folder+'/sources.tar.gz')
prefix = 'cd '+shlex.quote(folder)+' && '
status,_=command(prefix+'tar -xzf sources.tar.gz && g++ --version',10)
if status:
    raise RuntimeError('extract/compiler failed')
includes='-ICore/MainMenu/include -ICore/Convert/include -ICore/Web/include -IInput/WirelessCarPlay/include -IInput/WirelessCarLifePlus/include'
core='Core/MainMenu/src/session_core.cpp Core/Convert/src/real_media_store.cpp'
cp=core+' Input/WirelessCarPlay/src/catplay_media_client.cpp'
cl=core+' '+' '.join('Input/WirelessCarLifePlus/src/'+name+'.cpp' for name in (
    'wire','bt_link','wifi_ap','transport','session','encryption','keycode_map',
    'spp_profile','hfp_profile','hfp_slc','pairing_agent','bt_client_profile','carlife_input'))
results=[]
try:
    for name,sources,test,flags in (
        ('carplay-api',cp,'Input/WirelessCarPlay/tests/input_api_tests.cpp',''),
        ('carplay-ipc',cp,'Input/WirelessCarPlay/tests/media_ext_tests.cpp',''),
        ('carlife-api',cl,'Input/WirelessCarLifePlus/tests/input_api_tests.cpp','-DCARLIFE_HAVE_OPENSSL=1 -DCARLIFE_HAVE_SD_BUS=1 -lcrypto -lsystemd'),
    ):
        print('\nBUILD '+name,flush=True)
        status,log=command(prefix+f'timeout 180s nice -n 10 g++ -std=c++20 -O0 -pthread {includes} {sources} {test} {flags} -o {name}',185)
        if status:
            results.append({'test':name,'build_exit':status,'log':log})
            continue
        status,log=command(prefix+'timeout 30s ./'+name,35)
        results.append({'test':name,'build_exit':0,'test_exit':status,'log':log})
finally:
    client.close()
    (root/'Input/API_AUDIT/board-results.json').write_text(json.dumps({'directory':folder,'results':results},ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'directory':folder,'results':[{k:v for k,v in r.items() if k!='log'} for r in results]},indent=2))
sys.exit(0 if len(results)==3 and all(r.get('test_exit')==0 for r in results) else 1)
