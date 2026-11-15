# Cliente de sys-botbase para PC

Cliente local sin dependencias adicionales, usando el Python ya disponible en
este PC. El servidor se instala en la Switch, no en Windows. No requiere abrir
un puerto entrante en el PC. Compatible con el protocolo oficial TCP/CRLF de
[sys-botbase](https://github.com/olliz0r/sys-botbase).

Desde PowerShell en esta carpeta, sustituye la IP de ejemplo por la de tu consola:

```powershell
./Sysbot-PC.ps1 --host 192.168.1.123 configure
./Sysbot-PC.ps1 status
./Sysbot-PC.ps1 screenshot
./Sysbot-PC.ps1 click A
./Sysbot-PC.ps1 hold PLUS 0.2
./Sysbot-PC.ps1 stick LEFT 0 20000 1
```

El puerto predeterminado es 6000; se puede configurar con `--port` antes del
subcomando. `status` consulta version e identificador de la aplicacion.
`screenshot` guarda un JPEG en `capturas/`. `hold` suelta el boton al terminar y
`stick` vuelve a cero. La duracion maxima por accion es de 30 segundos.

La configuracion se guarda en `sysbot.json`. Hasta indicar la IP real, la
conexion fisica queda pendiente. Las pruebas locales usan un servidor TCP de
prueba en localhost y no prueban una Switch real.

## Transferencia por Wi-Fi

La consola de pruebas utiliza FTP en el puerto 5000. Para enviar un NRO,
cierra primero Celosia desde HOME. El envio comprueba el SHA-256 descargando
el archivo temporal antes de activarlo y conserva el NRO anterior como copia.

```powershell
python wireless.py push-nro ruta/al/celosia_switch_game.nro
python wireless.py pull-logs ruta/a/los/logs-de-la-prueba
```

Los logs principales se descargan al cerrar el juego porque permanecen abiertos
durante la ejecucion. Los archivos `hardware-status.txt` y `hardware-clocks.txt`
de las compilaciones nuevas permiten consultar FPS, resolucion y relojes mientras
el juego esta en marcha. La lectura de relojes no cambia sus valores.

Si `pixelPeek` devuelve una captura vacia, el cliente lo informa; no guarda una
imagen falsa. La disponibilidad de esa funcion depende de la configuracion del
sysmodule en la consola.

Pruebas:

```powershell
python -m unittest discover -s . -p test_sysbot.py -v
```
