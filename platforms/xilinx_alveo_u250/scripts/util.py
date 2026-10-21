from pathlib import Path
import subprocess

from typing import List, Tuple

def call_vivado(vivado: Path, args: List[str]) -> Tuple[int, str, str]:
    pVivado = subprocess.Popen(
        [
            str(vivado),
            # Interactive Tcl mode can return zero after a sourced script fails
            # and stdin reaches EOF. Batch mode propagates script errors.
            '-mode', 'batch',
        ] + args,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    sout, serr = pVivado.communicate()

    eSout = sout.decode('utf-8') if sout is not None else ""
    eSerr = serr.decode('utf-8') if serr is not None else ""

    return (pVivado.returncode, eSout, eSerr)
