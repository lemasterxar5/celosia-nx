import ftplib
import io
from pathlib import Path
import tempfile
import unittest
from wireless import upload_verified, pull_logs
from sysbot import ProtocolError

class FakeFTP:
    def __init__(self):
        self.files={'/switch/celosia-native-boot/celosia_switch_game.nro':b'old'}
        self.fail_install=False
    def storbinary(self,command,file,blocksize=8192):self.files[command[5:]]=file.read()
    def retrbinary(self,command,callback):callback(self.files[command[5:]])
    def rename(self,source,destination):
        if self.fail_install and source.endswith('.upload'):
            raise ftplib.error_perm('550 test failure')
        if source not in self.files:raise ftplib.error_perm('550 not found')
        self.files[destination]=self.files.pop(source)
    def nlst(self,path):return [p for p in self.files if p.startswith(path+'/')]

class WirelessTests(unittest.TestCase):
    def test_verified_install_and_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'game.nro';source.write_bytes(bytes(16)+b'NRO0'+bytes(128))
            ftp=FakeFTP();report=upload_verified(ftp,source)
            self.assertEqual(ftp.files[report['remote']],source.read_bytes())
            self.assertEqual(ftp.files[report['backup']],b'old')
    def test_failed_install_rolls_back(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'game.nro';source.write_bytes(bytes(16)+b'NRO0'+bytes(128))
            ftp=FakeFTP();ftp.fail_install=True
            with self.assertRaises(ftplib.error_perm):upload_verified(ftp,source)
            self.assertEqual(ftp.files['/switch/celosia-native-boot/celosia_switch_game.nro'],b'old')
    def test_invalid_nro_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'game.nro';source.write_bytes(b'not an NRO')
            with self.assertRaises(ValueError):upload_verified(FakeFTP(),source)
    def test_log_download(self):
        with tempfile.TemporaryDirectory() as directory:
            ftp=FakeFTP();ftp.files['/switch/celosia-native-boot/hybrid-error.log']=b'actual log'
            self.assertEqual(pull_logs(ftp,Path(directory)),['hybrid-error.log'])
            self.assertEqual((Path(directory)/'hybrid-error.log').read_bytes(),b'actual log')
    def test_app_started_during_upload_keeps_previous_nro(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'game.nro';source.write_bytes(bytes(16)+b'NRO0'+bytes(128))
            ftp=FakeFTP()
            def running():raise ValueError('application started')
            with self.assertRaises(ValueError):upload_verified(ftp,source,running)
            self.assertEqual(ftp.files['/switch/celosia-native-boot/celosia_switch_game.nro'],b'old')
    def test_bad_remote_hash_does_not_activate(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'game.nro';source.write_bytes(bytes(16)+b'NRO0'+bytes(128))
            ftp=FakeFTP();ftp.retrbinary=lambda command,callback:callback(b'corrupt')
            with self.assertRaises(ProtocolError):upload_verified(ftp,source)
            self.assertEqual(ftp.files['/switch/celosia-native-boot/celosia_switch_game.nro'],b'old')

if __name__=='__main__':unittest.main()
