"""Apply the minimal verified-TLS guard to the pinned SDK, only for RickyOS.

Like other dependency build patches, this is a bounded mechanical rewrite.
Unexpected upstream source fails the build instead of silently disabling trust.
The parent repository owns this patch; no SDK commit or remote write is needed.
"""
from pathlib import Path

OLD_TRUST = '''  } else if (_rootCA) {
    wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                   strlen(_rootCA), WOLFSSL_FILETYPE_PEM);
  }
'''
NEW_TRUST = '''  } else {
    // RICKYOS_VERIFIED_TLS: trust loading must succeed; never fall back to insecure.
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
    if (!_rootCA || wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                                 strlen(_rootCA), WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
      stop();
      return 0;
    }
  }
'''
OLD_HOST = '''  _ssl = ssl;
  wolfSSL_SetIOReadCtx(ssl, &_transport);
'''
NEW_HOST = '''  _ssl = ssl;
  if (!_insecure && wolfSSL_check_domain_name(ssl, host) != WOLFSSL_SUCCESS) {
    stop();
    return 0;
  }
  wolfSSL_SetIOReadCtx(ssl, &_transport);
'''


def patch_text(source):
    if 'RICKYOS_VERIFIED_TLS' in source:
        if NEW_TRUST not in source or NEW_HOST not in source:
            raise RuntimeError('Incomplete RickyOS TLS patch; inspect SDK before building.')
        return source
    if source.count(OLD_TRUST) != 1 or source.count(OLD_HOST) != 1:
        raise RuntimeError('SDK TLS source changed; review CA/hostname verification before building.')
    return source.replace(OLD_TRUST, NEW_TRUST).replace(OLD_HOST, NEW_HOST)


def apply(project_dir):
    path = Path(project_dir) / 'freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp'
    source = path.read_text()
    patched = patch_text(source)
    if patched != source:
        path.write_text(patched)


if 'Import' in globals():
    Import('env')
    if env.subst('$PIOENV') == 'rickyos_readpico':
        apply(env.subst('$PROJECT_DIR'))
elif __name__ == '__main__':
    apply(Path(__file__).resolve().parents[1])
