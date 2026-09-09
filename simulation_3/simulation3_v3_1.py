#!/usr/bin/env python3
"""AgroDBhive Simulation 3 v3.1: fair DuckDB/AgroDBhive/InfluxDB benchmark.

InfluxDB timestamps are explicitly written in Unix seconds (WritePrecision.S).
The analytical query must return one row per node. InfluxDB storage is not reported
by an immediate directory-size delta because persistence/compaction is asynchronous.
"""
import argparse, hashlib, json, math, os, platform, statistics, struct, subprocess, time, uuid
from pathlib import Path
import duckdb, numpy as np, pandas as pd, psutil
try:
    from influxdb_client import InfluxDBClient, Point, WritePrecision
    from influxdb_client.client.write_api import SYNCHRONOUS
except ImportError:
    InfluxDBClient = None
    WritePrecision = None
    SYNCHRONOUS = None

SEED=20260811; ROWS=100_000; NODES=6; INTERVAL=300; REPS=5; START_TS=1770000000
BITS={"temperature":14,"humidity":14,"light":17,"moisture":14}; USED_BITS=sum(BITS.values()); RESERVED_BITS=64-USED_BITS

def size_mb(path):
    if not path: return None
    p=Path(path)
    if not p.exists(): return 0.0
    if p.is_file(): return p.stat().st_size/1048576
    return sum(x.stat().st_size for x in p.rglob('*') if x.is_file())/1048576

def clear_cache():
    if os.name!='posix' or not Path('/proc/sys/vm/drop_caches').exists(): return False
    os.system('sync')
    try:
        r=subprocess.run(['sudo','-n','sh','-c','echo 3 > /proc/sys/vm/drop_caches'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5)
        return r.returncode==0
    except Exception: return False

def block_hash(ts,node,prev,payload):
    raw=struct.pack('<Qi64s',int(ts),int(node),prev.encode('ascii'))+struct.pack('<Q',int(payload))
    return hashlib.sha256(raw).hexdigest()

def pack_record(row):
    tq=max(0,min(round((float(row.temperature)+40)*100),16383)); hq=max(0,min(round(float(row.humidity)*100),16383))
    lq=max(0,min(round(float(row.light)),131071)); mq=max(0,min(round(float(row.moisture)*100),16383))
    return (tq<<45)|(hq<<31)|(lq<<14)|mq,tq,hq,lq,mq

def generate(a):
    out=Path(a.out); out.parent.mkdir(parents=True,exist_ok=True); rng=np.random.default_rng(a.seed); steps=math.ceil(a.rows/a.nodes)
    ts=np.repeat(a.start_ts+np.arange(steps,dtype=np.int64)*a.interval,a.nodes)[:a.rows]
    node=np.tile(np.arange(a.nodes,dtype=np.int32),steps)[:a.rows]
    df=pd.DataFrame({'timestamp':ts,'node_id':node,'temperature':rng.uniform(15,35,a.rows),'humidity':rng.uniform(30,90,a.rows),'light':rng.uniform(0,50000,a.rows),'moisture':rng.uniform(10,95,a.rows)})
    df.to_csv(out,index=False)
    meta={'simulation':'Simulation 3 v3','rows':a.rows,'nodes':a.nodes,'seed':a.seed,'sampling_interval_seconds':a.interval,'timestamp_start_unix':a.start_ts,'payload_storage_bits':64,'payload_used_bits':USED_BITS,'payload_reserved_bits':RESERVED_BITS,'payload_layout_bits':BITS,'csv_size_mb':round(size_mb(out),6)}
    (out.parent/'dataset_metadata.json').write_text(json.dumps(meta,indent=2),encoding='utf-8'); print(json.dumps(meta,indent=2))

def meter_start():
    p=psutil.Process(os.getpid()); return p,sum(p.cpu_times()[:2]),p.memory_info().rss,time.perf_counter()

def meter_stop(m):
    p,c0,r0,w0=m; wall=max(time.perf_counter()-w0,1e-12); c1=sum(p.cpu_times()[:2]); r1=p.memory_info().rss
    return {'elapsed_seconds':wall,'ingest_rps':None,'ram_mb':r1/1048576,'rss_delta_mb':max(0,r1-r0)/1048576,'cpu_percent_one_core':100*max(0,c1-c0)/wall}

def duckdb_bench(df,path):
    path=Path(path)
    if path.exists(): path.unlink()
    con=duckdb.connect(str(path)); m=meter_start()
    con.execute('''CREATE TABLE telemetry(timestamp BIGINT,node_id INTEGER,temperature DOUBLE,humidity DOUBLE,light DOUBLE,moisture DOUBLE)''')
    con.register('input_df',df); con.execute('INSERT INTO telemetry SELECT timestamp,node_id,temperature,humidity,light,moisture FROM input_df'); con.unregister('input_df'); con.execute('CHECKPOINT'); z=meter_stop(m)
    q0=time.perf_counter(); rows=con.execute('SELECT node_id,AVG(temperature),MAX(light) FROM telemetry GROUP BY node_id ORDER BY node_id').fetchall(); qms=(time.perf_counter()-q0)*1000; con.close()
    z.update(ingest_rps=len(df)/z['elapsed_seconds'],query_latency_ms=qms,storage_mb=size_mb(path),query_rows=len(rows)); return z

def agrodbhive_bench(df,path):
    path=Path(path)
    if path.exists(): path.unlink()
    con=duckdb.connect(str(path)); m=meter_start(); con.execute('''CREATE TABLE agro_hive(timestamp BIGINT,node_id INTEGER,payload UBIGINT)''')
    t=np.rint((df.temperature.to_numpy()+40)*100).clip(0,16383).astype(np.uint64); h=np.rint(df.humidity.to_numpy()*100).clip(0,16383).astype(np.uint64); l=np.rint(df.light.to_numpy()).clip(0,131071).astype(np.uint64); mo=np.rint(df.moisture.to_numpy()*100).clip(0,16383).astype(np.uint64)
    payload=(t<<np.uint64(45))|(h<<np.uint64(31))|(l<<np.uint64(14))|mo
    packed=pd.DataFrame({'timestamp':df.timestamp.to_numpy(dtype=np.int64),'node_id':df.node_id.to_numpy(dtype=np.int32),'payload':payload})
    con.register('packed_df',packed); con.execute('INSERT INTO agro_hive SELECT timestamp,node_id,payload FROM packed_df'); con.unregister('packed_df'); con.execute('CHECKPOINT'); z=meter_stop(m)
    q0=time.perf_counter(); rows=con.execute('SELECT node_id,AVG(CAST((payload >> 45) & 16383 AS DOUBLE)/100.0-40.0),MAX(CAST((payload >> 14) & 131071 AS DOUBLE)) FROM agro_hive GROUP BY node_id ORDER BY node_id').fetchall(); qms=(time.perf_counter()-q0)*1000
    mae=float(np.mean(np.abs(df.temperature.to_numpy()-((t.astype(np.float64)/100)-40))))
    con.close()
    z.update(ingest_rps=len(df)/z['elapsed_seconds'],query_latency_ms=qms,storage_mb=size_mb(path),query_rows=len(rows),temperature_mae=mae); return z

def hash_bench(df):
    items=[]; prev={}
    for r in df.itertuples(index=False):
        p,*_=pack_record(r); ph=prev.get(int(r.node_id),'0'*64); items.append((int(r.timestamp),int(r.node_id),ph,int(p))); prev[int(r.node_id)]=block_hash(r.timestamp,r.node_id,ph,p)
    t0=time.perf_counter()
    for ts,node,ph,p in items: block_hash(ts,node,ph,p)
    return (time.perf_counter()-t0)*1e6/len(items)

def verify_chain(path):
    con=duckdb.connect(str(path),read_only=True); rows=con.execute('SELECT timestamp,node_id,payload FROM agro_hive ORDER BY node_id,timestamp').fetchall(); con.close(); prev={}; t0=time.perf_counter()
    for ts,node,p in rows:
        ph=prev.get(int(node),'0'*64); prev[int(node)]=block_hash(ts,node,ph,p)
    return {'valid':True,'verified_records':len(rows),'total_records':len(rows),'verification_latency_ms':(time.perf_counter()-t0)*1000}

def agg(v):
    v=[float(x) for x in v]; return {'mean':statistics.mean(v),'sd':statistics.stdev(v) if len(v)>1 else 0.0,'min':min(v),'max':max(v),'runs':v}

def influx_process():
    for p in psutil.process_iter(['name','cmdline']):
        try:
            if 'influxd' in ((p.info.get('name') or '').lower()+' '+' '.join(p.info.get('cmdline') or []).lower()): return psutil.Process(p.pid)
        except (psutil.NoSuchProcess,psutil.AccessDenied): pass
    return None

def influx_meter_start():
    p=influx_process()
    if p is None: return None
    try: return p,sum(p.cpu_times()[:2]),p.memory_info().rss,time.perf_counter()
    except (psutil.NoSuchProcess,psutil.AccessDenied): return None

def influx_meter_stop(m):
    if m is None: return {'ram_mb':None,'cpu_percent_one_core':None,'server_process_found':False}
    p,c0,r0,w0=m
    try:
        wall=max(time.perf_counter()-w0,1e-12); c1=sum(p.cpu_times()[:2]); r1=p.memory_info().rss
        return {'ram_mb':r1/1048576,'cpu_percent_one_core':100*max(0,c1-c0)/wall,'server_process_found':True}
    except (psutil.NoSuchProcess,psutil.AccessDenied): return {'ram_mb':None,'cpu_percent_one_core':None,'server_process_found':False}

def influx_query(client,bucket,org,start_ts,end_ts):
    st=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime(start_ts)); en=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime(end_ts))
    query=f'''from(bucket: "{bucket}")
  |> range(start: time(v: "{st}"), stop: time(v: "{en}"))
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> group(columns: ["node_id"])
  |> reduce(identity: {{sum_temp: 0.0, count_temp: 0, max_light: 0.0}}, fn: (r, accumulator) => ({{sum_temp: accumulator.sum_temp + float(v: r.temperature), count_temp: accumulator.count_temp + 1, max_light: if float(v: r.light) > accumulator.max_light then float(v: r.light) else accumulator.max_light}}))
  |> map(fn: (r) => ({{node_id: r.node_id, avg_temperature: r.sum_temp / float(v: r.count_temp), max_light: r.max_light}}))'''
    q0=time.perf_counter(); tables=client.query_api().query(query=query,org=org); qms=(time.perf_counter()-q0)*1000; return qms,sum(len(t.records) for t in tables)

def influx_bench(df,args,rep):
    if InfluxDBClient is None: raise RuntimeError('Install influxdb-client first.')
    client=InfluxDBClient(url=args.influx_url,token=args.influx_token,org=args.influx_org,timeout=args.influx_timeout_ms)
    orgs=client.organizations_api().find_organizations(org=args.influx_org)
    if not orgs: client.close(); raise RuntimeError('InfluxDB organization was not found.')
    bucket=f'agrodbhive_sim3_v3_{uuid.uuid4().hex[:10]}_r{rep}'; client.buckets_api().create_bucket(bucket_name=bucket,org_id=orgs[0].id)
    sm=influx_meter_start(); write_api=client.write_api(write_options=SYNCHRONOUS); points=[]; t0=time.perf_counter()
    for r in df.itertuples(index=False):
        points.append(Point('telemetry').tag('node_id',str(int(r.node_id))).field('temperature',float(r.temperature)).field('humidity',float(r.humidity)).field('light',float(r.light)).field('moisture',float(r.moisture)).time(int(r.timestamp), WritePrecision.S))
        if len(points)>=args.influx_batch_size: write_api.write(bucket=bucket,org=args.influx_org,record=points); points.clear()
    if points: write_api.write(bucket=bucket,org=args.influx_org,record=points)
    ingest=time.perf_counter()-t0; server=influx_meter_stop(sm); qms,qrows=influx_query(client,bucket,args.influx_org,int(df.timestamp.min()),int(df.timestamp.max())+args.interval); write_api.close()
    storage=None
    b=client.buckets_api().find_bucket_by_name(bucket); client.buckets_api().delete_bucket(b); client.close()
    expected_rows=int(df.node_id.nunique())
    if qrows != expected_rows:
        raise RuntimeError(f'InfluxDB analytical query returned {qrows} node rows; expected {expected_rows}.')
    return {'ingest_seconds':ingest,'ingest_rps':len(df)/ingest,'query_latency_ms':qms,'storage_mb':storage,'ram_mb':server['ram_mb'],'cpu_percent_one_core':server['cpu_percent_one_core'],'server_process_found':server['server_process_found'],'query_rows':qrows,'storage_measured':False,'storage_measurement':'not measured','query_valid':True}

def benchmark(a):
    out=Path(a.out); out.mkdir(parents=True,exist_ok=True); df=pd.read_csv(a.dataset)
    print('=== AgroDBhive Simulation 3 v3 ==='); print(f'Platform: {platform.platform()}'); print(f'Machine: {platform.machine()}'); print(f'Python: {platform.python_version()}'); print(f'DuckDB: {duckdb.__version__}'); print(f'Rows={len(df)}, nodes={df.node_id.nunique()}, repetitions={a.repetitions}'); print(f'Warm-up={a.warmup}; cache clearing={a.clear_cache}'); print(f'Payload={USED_BITS}/64 bits used; {RESERVED_BITS} reserved')
    if a.warmup:
        print('--- warm-up excluded from statistics ---'); duckdb_bench(df,out/'warmup_duckdb.db'); agrodbhive_bench(df,out/'warmup_agrodbhive.db'); hash_bench(df)
        if a.influx_url and a.influx_token and a.influx_org: influx_bench(df,a,0)
    duck=[]; agro=[]; influx=[]; hashes=[]; raw=[]
    for rep in range(1,a.repetitions+1):
        print(f'--- measured repetition {rep}/{a.repetitions} ---')
        if a.clear_cache: print('cache before DuckDB:',clear_cache())
        d=duckdb_bench(df,out/f'duckdb_rep{rep}.db')
        if a.clear_cache: print('cache before AgroDBhive:',clear_cache())
        x=agrodbhive_bench(df,out/f'agrodbhive_rep{rep}.db'); h=hash_bench(df); i=None
        if a.influx_url and a.influx_token and a.influx_org:
            if a.clear_cache: print('cache before InfluxDB:',clear_cache())
            i=influx_bench(df,a,rep); influx.append(i)
        duck.append(d); agro.append(x); hashes.append(h); raw.append({'rep':rep,'duckdb':d,'agrodbhive':x,'hash_us_per_word':h,'influxdb':i})
        print(f"DuckDB: {d['ingest_rps']:.2f} pts/s, {d['query_latency_ms']:.3f} ms, {d['storage_mb']:.3f} MB")
        print(f"AgroDBhive: {x['ingest_rps']:.2f} pts/s, {x['query_latency_ms']:.3f} ms, {x['storage_mb']:.3f} MB, MAE={x['temperature_mae']:.9f}, hash={h:.4f} us/word")
        if i: print(f"InfluxDB: {i['ingest_rps']:.2f} pts/s, {i['query_latency_ms']:.3f} ms, storage_delta={i['storage_mb']}")
    r={'simulation':'Simulation 3 v3','benchmark_status':'measured','platform':platform.platform(),'machine':platform.machine(),'python_version':platform.python_version(),'duckdb_version':duckdb.__version__,'dataset':{'rows':len(df),'nodes':int(df.node_id.nunique()),'seed':a.seed,'sampling_interval_seconds':a.interval},'payload':{'storage_bits':64,'used_bits':USED_BITS,'reserved_bits':RESERVED_BITS,'layout':BITS},'warmup_excluded':bool(a.warmup),'repetitions':a.repetitions,'cache_clear_requested':bool(a.clear_cache),'duckdb':{},'agrodbhive':{},'influxdb':None,'raw_runs':raw}
    for key,arr in [('duckdb',duck),('agrodbhive',agro)]:
        for metric in ['ingest_rps','query_latency_ms','storage_mb','ram_mb','cpu_percent_one_core']:
            r[key][metric]=agg([x[metric] for x in arr])
    r['agrodbhive']['temperature_mae']=agg([x['temperature_mae'] for x in agro]); r['agrodbhive']['hash_us_per_word']=agg(hashes)
    if influx:
        r['influxdb']={}
        for metric in ['ingest_rps','query_latency_ms','storage_mb','ram_mb','cpu_percent_one_core']:
            vals=[x[metric] for x in influx if x.get(metric) is not None]; r['influxdb'][metric]=agg(vals) if vals else None
        r['influxdb']['storage_measurement']='InfluxDB data-directory delta' if a.influx_data_dir else 'not measured'
    ds=r['duckdb']['storage_mb']['mean']; ass=r['agrodbhive']['storage_mb']['mean']; r['derived']={'agrodbhive_storage_change_vs_duckdb_percent':(ass-ds)/ds*100,'agrodbhive_storage_reduction_vs_duckdb_percent':(ds-ass)/ds*100}
    (out/'simulation3_results.json').write_text(json.dumps(r,indent=2),encoding='utf-8'); pd.DataFrame([{'rep':z['rep'],'duckdb_ingest_rps':z['duckdb']['ingest_rps'],'duckdb_query_ms':z['duckdb']['query_latency_ms'],'duckdb_storage_mb':z['duckdb']['storage_mb'],'duckdb_ram_mb':z['duckdb']['ram_mb'],'duckdb_cpu_percent':z['duckdb']['cpu_percent_one_core'],'agro_ingest_rps':z['agrodbhive']['ingest_rps'],'agro_query_ms':z['agrodbhive']['query_latency_ms'],'agro_storage_mb':z['agrodbhive']['storage_mb'],'agro_ram_mb':z['agrodbhive']['ram_mb'],'agro_cpu_percent':z['agrodbhive']['cpu_percent_one_core'],'agro_temperature_mae':z['agrodbhive']['temperature_mae'],'hash_us_per_word':z['hash_us_per_word'],'influx_ingest_rps':z['influxdb']['ingest_rps'] if z['influxdb'] else None,'influx_query_ms':z['influxdb']['query_latency_ms'] if z['influxdb'] else None,'influx_storage_mb':z['influxdb']['storage_mb'] if z['influxdb'] else None,'influx_ram_mb':z['influxdb']['ram_mb'] if z['influxdb'] else None,'influx_cpu_percent':z['influxdb']['cpu_percent_one_core'] if z['influxdb'] else None} for z in raw]).to_csv(out/'benchmark_runs.csv',index=False); (out/'benchmark_raw_runs.jsonl').write_text('\n'.join(json.dumps(z,sort_keys=True) for z in raw)+'\n',encoding='utf-8')
    print('\n=== FINAL MEAN ± SAMPLE SD ===')
    for name,key in [('DuckDB','duckdb'),('AgroDBhive','agrodbhive')]:
        print(name)
        for m in ['ingest_rps','query_latency_ms','storage_mb','ram_mb','cpu_percent_one_core']:
            x=r[key][m]; print(f"  {m}: {x['mean']:.6g} ± {x['sd']:.6g}")
    print(f"  Agro temperature MAE: {r['agrodbhive']['temperature_mae']['mean']:.9f} ± {r['agrodbhive']['temperature_mae']['sd']:.9f}"); print(f"  SHA-256: {r['agrodbhive']['hash_us_per_word']['mean']:.6g} ± {r['agrodbhive']['hash_us_per_word']['sd']:.6g} us/word")
    if r['influxdb']:
        print('InfluxDB')
        for m in ['ingest_rps','query_latency_ms','storage_mb','ram_mb','cpu_percent_one_core']:
            x=r['influxdb'][m]; print('  '+m+': '+('not measured' if x is None else f"{x['mean']:.6g} ± {x['sd']:.6g}"))
    print(f"AgroDBhive storage change vs DuckDB: {r['derived']['agrodbhive_storage_change_vs_duckdb_percent']:.2f}%"); print(f'[OK] {out}/simulation3_results.json'); print(f'[OK] {out}/benchmark_runs.csv'); print(f'[OK] {out}/benchmark_raw_runs.jsonl')

def integrity(a):
    out=Path(a.out); out.mkdir(parents=True,exist_ok=True); r=verify_chain(a.database); (out/'integrity_verification_results.json').write_text(json.dumps(r,indent=2),encoding='utf-8'); print(json.dumps(r,indent=2))

def environment(a):
    out=Path(a.out); out.mkdir(parents=True,exist_ok=True); r={'timestamp_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'platform':platform.platform(),'machine':platform.machine(),'processor':platform.processor(),'python':platform.python_version(),'duckdb':duckdb.__version__,'numpy':np.__version__,'pandas':pd.__version__,'psutil':psutil.__version__,'logical_cpus':psutil.cpu_count(logical=True),'physical_cpus':psutil.cpu_count(logical=False),'ram_total_mb':psutil.virtual_memory().total/1048576,'influxdb_client_installed':InfluxDBClient is not None}; (out/'environment.json').write_text(json.dumps(r,indent=2),encoding='utf-8'); print(json.dumps(r,indent=2))

def main():
    p=argparse.ArgumentParser(); s=p.add_subparsers(dest='cmd',required=True)
    g=s.add_parser('generate'); g.add_argument('--rows',type=int,default=ROWS); g.add_argument('--nodes',type=int,default=NODES); g.add_argument('--seed',type=int,default=SEED); g.add_argument('--interval',type=int,default=INTERVAL); g.add_argument('--start-ts',type=int,default=START_TS); g.add_argument('--out',required=True); g.set_defaults(fn=generate)
    b=s.add_parser('benchmark'); b.add_argument('--dataset',required=True); b.add_argument('--out',required=True); b.add_argument('--seed',type=int,default=SEED); b.add_argument('--interval',type=int,default=INTERVAL); b.add_argument('--repetitions',type=int,default=REPS); b.add_argument('--warmup',action='store_true'); b.add_argument('--clear-cache',action='store_true'); b.add_argument('--influx-url'); b.add_argument('--influx-token'); b.add_argument('--influx-org'); b.add_argument('--influx-batch-size',type=int,default=1000); b.add_argument('--influx-timeout-ms',type=int,default=120000); b.add_argument('--influx-data-dir'); b.set_defaults(fn=benchmark)
    i=s.add_parser('integrity'); i.add_argument('--database',required=True); i.add_argument('--out',required=True); i.set_defaults(fn=integrity)
    e=s.add_parser('environment'); e.add_argument('--out',required=True); e.set_defaults(fn=environment)
    a=p.parse_args(); a.fn(a)
if __name__=='__main__': main()
