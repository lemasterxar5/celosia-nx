"""Wireless NRO deployment and log collection via the Switch FTP sysmodule."""
import argparse
import ftplib
import hashlib
import json
from pathlib import Path, PurePosixPath
import uuid
from sysbot import SysBot, validate_endpoint, ProtocolError

ROOT='/switch/celosia_nx'
REMOTE=ROOT+'/celosia_nx.nro'

def upload_verified(ftp,source,before_activate=None):
    source=Path(source)
    if not 128<=source.stat().st_size<=256*1024*1024:
        raise ValueError('Tamano de NRO invalido')
    with source.open('rb') as file:
        file.seek(16)
        if file.read(4)!=b'NRO0':raise ValueError('El archivo no tiene cabecera NRO')
        file.seek(0);expected=hashlib.file_digest(file,'sha256').hexdigest()
        file.seek(0);ftp.storbinary('STOR '+REMOTE+'.upload',file,blocksize=262144)
    actual=hashlib.sha256()
    ftp.retrbinary('RETR '+REMOTE+'.upload',actual.update)
    if actual.hexdigest()!=expected:
        raise ProtocolError('SHA256 remoto diferente; se conserva el NRO anterior')
    if before_activate:before_activate()
    names={PurePosixPath(name).name for name in ftp.nlst(ROOT)}
    backup=REMOTE+'.previous-'+uuid.uuid4().hex[:12] if PurePosixPath(REMOTE).name in names else None
    if backup:ftp.rename(REMOTE,backup)
    try:
        ftp.rename(REMOTE+'.upload',REMOTE)
    except BaseException:
        if backup:ftp.rename(backup,REMOTE)
        raise
    return {'remote':REMOTE,'backup':backup,'sha256':expected,'bytes':source.stat().st_size}

def pull_logs(ftp,directory):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    names={PurePosixPath(name).name for name in ftp.nlst(ROOT)}
    downloaded=[]
    for name in ('hybrid-boot.log','hybrid-error.log','hardware-status.txt','hardware-clocks.txt','render-progress.txt','native-fault.txt'):
        if name not in names:continue
        temporary=directory/(name+'.partial')
        try:
            with temporary.open('wb') as file:ftp.retrbinary('RETR '+ROOT+'/'+name,file.write)
        except ftplib.error_temp as error:
            print(f'{name}: no disponible mientras esta abierto ({error})')
            continue
        temporary.replace(directory/name);downloaded.append(name)
    return downloaded

def main():
    parser=argparse.ArgumentParser(description='Enviar NRO y recoger logs por Wi-Fi')
    parser.add_argument('--config',type=Path,default=Path(__file__).with_name('sysbot.json'))
    parser.add_argument('--ftp-port',type=int,default=5000)
    actions=parser.add_subparsers(dest='action',required=True)
    push=actions.add_parser('push-nro');push.add_argument('source',type=Path)
    pull=actions.add_parser('pull-logs');pull.add_argument('directory',type=Path)
    args=parser.parse_args()
    try:
        config=json.loads(args.config.read_text(encoding='utf-8-sig'))
        if not isinstance(config,dict):raise ValueError('Configuracion JSON invalida')
        host=config.get('host');validate_endpoint(host,args.ftp_port)
        def ensure_stopped():
            with SysBot(host,config.get('port',6000)) as client:
                if client.status()['title_id'] is not None:
                    raise ValueError('Cierra la aplicacion desde HOME antes de sustituir el NRO')
        if args.action=='push-nro':ensure_stopped()
        with ftplib.FTP() as ftp:
            ftp.connect(host,args.ftp_port,timeout=15);ftp.login()
            result=upload_verified(ftp,args.source,ensure_stopped) if args.action=='push-nro' else pull_logs(ftp,args.directory)
        print(json.dumps(result,indent=2));return 0 if result else 1
    except (OSError,ValueError,ProtocolError,ftplib.Error) as error:
        print('ERROR: '+str(error));return 1

if __name__=='__main__':raise SystemExit(main())
