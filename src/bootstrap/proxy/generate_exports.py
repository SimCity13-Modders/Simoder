"""@summary Generate the x86 D3D9 export contract and transparent stack-preserving thunks."""
import pathlib
import sys

# /** @summary Preserve Windows D3D9 ordinals, including exports accessed without names. */
names = {
    20: 'Direct3DCreate9On12', 21: 'Direct3DCreate9On12Ex',
    24: 'Direct3DShaderValidatorCreate9', 25: 'PSGPError', 26: 'PSGPSampleTexture',
    27: 'D3DPERF_BeginEvent', 28: 'D3DPERF_EndEvent', 29: 'D3DPERF_GetStatus',
    30: 'D3DPERF_QueryRepeatFrame', 31: 'D3DPERF_SetMarker', 32: 'D3DPERF_SetOptions',
    33: 'D3DPERF_SetRegion', 34: 'DebugSetLevel', 35: 'DebugSetMute',
    36: 'Direct3D9EnableMaximizedWindowedModeShim',
    37: 'Direct3DCreate9', 38: 'Direct3DCreate9Ex',
}
output = pathlib.Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
definitions = ['; @summary Export the x86 system D3D9 ABI through independent Simoder forwarding.', 'LIBRARY d3d9', 'EXPORTS']
assembly = ['; @summary Resolve once, preserve flags/registers/stack, then tail-call the genuine export.',
            '.386', '.model flat', 'EXTERN _g_d3dExports:DWORD',
            'EXTERN _ProxyResolveExport@4:PROC', '.code']
for ordinal in range(16, 39):
    name = names.get(ordinal, f'SimoderOrdinal{ordinal}')
    if ordinal == 37:
        target = '_SimoderDirect3DCreate9@4'
    elif ordinal == 38:
        target = '_SimoderDirect3DCreate9Ex@8'
    else:
        target = f'_SimoderProxyOrdinal{ordinal}'
        assembly.extend([f'PUBLIC {target}', f'{target} PROC', '    pushfd', '    pushad',
                         f'    push {ordinal - 16}', '    call _ProxyResolveExport@4',
                         '    popad', '    popfd', f'    jmp DWORD PTR [_g_d3dExports + {(ordinal - 16) * 4}]',
                         f'{target} ENDP'])
    # /** @summary Let LINK decorate cdecl thunk aliases; stdcall targets retain their explicit @ suffix. */
    internal = target if ordinal in (37, 38) else target.removeprefix('_')
    definitions.append(f'    {name}={internal} @{ordinal}' + (' NONAME' if ordinal not in names else ''))
assembly.append('END')
(output / 'd3d9.def').write_text('\n'.join(definitions) + '\n', encoding='utf-8')
(output / 'd3d9.asm').write_text('\n'.join(assembly) + '\n', encoding='utf-8')
