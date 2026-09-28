#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
model=QL-1060N
printer_uri=""

if [[ ${1:-} == "--ip" && -n ${2:-} ]]; then
  printer_uri="socket://${2}:9100"
elif [[ ${1:-} == "--usb" ]]; then
  printer_uri=$(lpinfo -v 2>/dev/null | awk '/usb:\/\/Brother\/QL-10(50|60N)/{print $2; exit}')
  [[ $printer_uri == *QL-1050* ]] && model=QL-1050
fi

queue_name=Brother_${model//-/_}

make -C "$script_dir" package
sudo install -d -m 755 /Library/Printers/QL1060N-macOS27/Filter
sudo install -m 755 "$script_dir/build/rastertoql1060n" /Library/Printers/QL1060N-macOS27/Filter/rastertoql1060n
sudo install -m 644 "$script_dir/ppd/Brother-QL-1060N-macOS27.ppd" "/Library/Printers/PPDs/Contents/Resources/Brother QL-1060N macOS27.ppd"
sudo install -m 644 "$script_dir/ppd/Brother-QL-1050-macOS27.ppd" "/Library/Printers/PPDs/Contents/Resources/Brother QL-1050 macOS27.ppd"

if [[ -n "$printer_uri" ]]; then
  sudo lpadmin -p "$queue_name" -E -v "$printer_uri" -P "/Library/Printers/PPDs/Contents/Resources/Brother ${model} macOS27.ppd"
  echo "Cola creada: $queue_name ($printer_uri)"
else
  echo "Controlador instalado. Ejecuta con --ip DIRECCION o --usb para crear la cola."
fi
