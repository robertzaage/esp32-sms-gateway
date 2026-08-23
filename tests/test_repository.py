from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RepositoryContractTests(unittest.TestCase):
    def test_partition_table_fits_8mb_without_overlap(self):
        rows = []
        for raw in (ROOT / "partitions.csv").read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            rows.append([part.strip() for part in line.split(",")])

        end = 0
        for name, _ptype, _subtype, offset, size, *_ in rows:
            start = int(offset, 0)
            length = int(size, 0)
            self.assertGreaterEqual(start, end, f"partition overlap at {name}")
            end = start + length
        self.assertEqual(end, 0x800000)

    def test_openapi_sms_send_contract_is_async_and_idempotent(self):
        text = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")
        section = text.split("/api/v1/messages:", 1)[1].split("/api/v1/messages/{messageId}:", 1)[0]
        self.assertIn('"202":', section)
        self.assertIn("Idempotency-Key", section)

    def test_toolchain_is_exactly_pinned(self):
        manifest = (ROOT / "main" / "idf_component.yml").read_text(encoding="utf-8")
        self.assertIn('idf: "==6.0.2"', manifest)
        self.assertIn('espressif/usb_host_cdc_acm: "==2.4.0"', manifest)


if __name__ == "__main__":
    unittest.main()
