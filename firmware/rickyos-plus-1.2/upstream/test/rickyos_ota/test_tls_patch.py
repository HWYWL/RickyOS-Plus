"""Check the pinned SDK trust patch and embedded CA; not a device handshake."""
import base64
import hashlib
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ricky_tls_patch", ROOT / "scripts/patch_rickyos_tls.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class TrustPatchTests(unittest.TestCase):
    def setUp(self):
        self.source = patch.OLD_TRUST + patch.OLD_HOST + "  wolfSSL_connect(ssl);\n"

    def test_applies_both_guards(self):
        updated = patch.patch_text(self.source)
        self.assertIn(patch.NEW_TRUST, updated)
        self.assertIn(patch.NEW_HOST, updated)
        self.assertNotIn(patch.OLD_TRUST, updated)
        self.assertLess(updated.index("wolfSSL_check_domain_name"), updated.index("wolfSSL_connect"))

    def test_idempotent(self):
        updated = patch.patch_text(self.source)
        self.assertEqual(updated, patch.patch_text(updated))

    def test_changed_or_duplicate_source_is_rejected(self):
        for source in ("unrecognized SDK", self.source + patch.OLD_TRUST, self.source.replace(patch.OLD_HOST, "")):
            with self.assertRaises(RuntimeError):
                patch.patch_text(source)

    def test_partial_patch_is_rejected(self):
        with self.assertRaises(RuntimeError):
            patch.patch_text(self.source.replace(patch.OLD_TRUST, patch.NEW_TRUST))

    def test_peer_verification_and_trust_load_failure_are_explicit(self):
        self.assertIn("wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER", patch.NEW_TRUST)
        self.assertIn("!_rootCA ||", patch.NEW_TRUST)
        self.assertIn("!= WOLFSSL_SUCCESS", patch.NEW_TRUST)
        self.assertIn("return 0;", patch.NEW_TRUST)

    def test_embedded_root_has_reviewed_fingerprint(self):
        source = (ROOT / "src/network/RickyOtaTrust.h").read_text()
        pem_body = re.search(r"-----BEGIN CERTIFICATE-----\s+(.*?)\s+-----END CERTIFICATE-----", source, re.S).group(1)
        der = base64.b64decode("".join(pem_body.split()), validate=True)
        self.assertEqual(hashlib.sha256(der).hexdigest(),
                         "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6")


if __name__ == "__main__":
    unittest.main()
