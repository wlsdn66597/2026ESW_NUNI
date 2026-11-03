"""Run inside dbus-run-session. Exercises actual ARM64 service method signatures."""
import argparse
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--sysroot')
    parser.add_argument('--extra-libs', default='')
    args = parser.parse_args()
    assert os.environ.get('DBUS_SESSION_BUS_ADDRESS'), 'run under dbus-run-session'
    base = int(time.time()) // 60 * 60
    with tempfile.TemporaryDirectory(prefix='nuni-dbus-') as directory:
        root = Path(directory)
        rows = []
        def add(topic, payload, offset):
            rows.append(dict(topic=topic, payload=payload, at=base+offset))
        for seq, temp in [(1,23),(2,25)]:
            add('sensor/env', dict(sample_id=f'esp:{seq}',source='pi4-usb',valid=True,age_s=0,temp=temp,humidity=45,co2=650),seq)
        add('sensor/radar',dict(boot_id='radar',seq=1,source='pi4-usb',valid=True,presence=True,breath_valid=True,breathing_rate=30,movement=0.2),2)
        add('audio/cry',dict(boot_id='mic',seq=1,ts=base+2,is_crying=False,confidence=0.1),2)
        for identifier, offset in [('first',3),('second',4)]:
            add('cry/episode',dict(id=identifier,event='end',t0=base+offset,t1=base+offset+1),offset+1)
        add('cry/feedback',dict(id='first',event='answer',action='feed',ts=base+5),5)
        add('control/action',dict(id='control-1',ts=base+5,on=True),5)
        fixture = root/'fixture.jsonl'
        fixture.write_text(''.join(json.dumps(row)+'\n' for row in rows))
        command = [args.server, str(root/'history.db'), str(fixture)]
        if args.sysroot:
            command = ['qemu-aarch64',args.sysroot+'/usr/lib/ld-linux-aarch64.so.1',
                       '--library-path',(args.extra_libs+':' if args.extra_libs else '')+args.sysroot+'/usr/lib'] + command
        with open(root/'server.log','w+') as log:
            server = subprocess.Popen(command,stdout=subprocess.PIPE,stderr=log,text=True)
            try:
                selector = selectors.DefaultSelector()
                selector.register(server.stdout,selectors.EVENT_READ)
                assert selector.select(timeout=20), 'server startup timeout'
                assert server.stdout.readline().strip() == 'ready', 'server failed to start'
                selector.close()
                def call(method, signature, *values):
                    command = ['busctl','--user','--json=short','--','call','com.atlas.NuniFusion1',
                        '/com/atlas/NuniFusion1','com.atlas.NuniFusion1',method,signature] + [str(v) for v in values]
                    result = subprocess.run(command,check=True,capture_output=True,text=True,timeout=10)
                    wire = json.loads(result.stdout)
                    assert wire['type'] == 's'
                    return json.loads(wire['data'][0])
                summary = call('GetHistorySummary','xxu',base,base+60,1)
                assert summary['ok'] and summary['total_count'] == 2 and summary['truncated']
                assert sum(summary['hourly']) == 2 and summary['timezone'] == 'Asia/Seoul'
                env = call('GetEnvironmentHistory','xxu',base,base+60,1000)
                assert env['records'][0]['temperature_c']['mean'] == 24
                sensors = call('GetSensorHistory','xxu',base,base+60,1000)
                assert len(sensors['records']) == 2
                events = call('GetRecordedEvents','xxux',base,base+60,2,0)
                assert len(events['records']) == 2 and events['truncated']
                rest = call('GetRecordedEvents','xxux',base,base+60,1000,events['after_id'])
                assert len(rest['records']) == 2
                assert not call('GetHistorySummary','xxu',base,base,1)['ok']
                assert not call('GetRecordedEvents','xxux',base,base+60,1,-1)['ok']
                server.terminate()
                assert server.wait(timeout=10) == 0
            except BaseException:
                log.flush();log.seek(0);print(log.read());raise
            finally:
                if server.poll() is None:
                    server.terminate()
                    try: server.wait(timeout=5)
                    except subprocess.TimeoutExpired: server.kill();server.wait()
    print('Actual session D-Bus: summary, environment, sensors, event cursor and errors OK')

if __name__ == '__main__':
    main()
