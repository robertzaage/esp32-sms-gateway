from pathlib import Path
import subprocess
import tempfile
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
        self.assertIn('espressif/cjson: "==1.7.19~2"', manifest)
        self.assertIn('espressif/mqtt: "==1.1.0"', manifest)

    def test_huawei_descriptor_candidate_ranking(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "descriptor_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "components" / "usb_modem_transport" / "include"),
                    str(ROOT / "components" / "usb_modem_transport" / "usb_modem_descriptor.c"),
                    str(ROOT / "tests" / "test_usb_descriptor.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_huawei_mode_switch_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "huawei_mode_switch_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "components" / "usb_modem_transport" / "include"),
                    str(ROOT / "components" / "usb_modem_transport" / "huawei_mode_switch.c"),
                    str(ROOT / "tests" / "test_huawei_mode_switch.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_modem_status_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "modem_status_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "components" / "modem_manager" / "include"),
                    str(ROOT / "components" / "modem_manager" / "modem_status.c"),
                    str(ROOT / "tests" / "test_modem_status.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_modem_recovery_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "modem_recovery_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "components" / "modem_manager" / "include"),
                    str(ROOT / "components" / "modem_manager" / "modem_recovery_policy.c"),
                    str(ROOT / "tests" / "test_modem_recovery_policy.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_sms_codec(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "sms_codec_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "components" / "sms_codec" / "include"),
                    str(ROOT / "components" / "sms_codec" / "sms_gsm7.c"),
                    str(ROOT / "components" / "sms_codec" / "sms_pdu.c"),
                    str(ROOT / "tests" / "test_sms_codec.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_sms_storage_partition_is_dedicated_nvs(self):
        text = (ROOT / "partitions.csv").read_text(encoding="utf-8")
        self.assertIn("storage,     data, nvs,", text)

    def test_sms_store_with_fake_nvs(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "sms_store_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "tests" / "host_stubs"),
                    "-I", str(ROOT / "components" / "sms_codec" / "include"),
                    "-I", str(ROOT / "components" / "sms_store" / "include"),
                    str(ROOT / "components" / "sms_store" / "sms_store.c"),
                    str(ROOT / "tests" / "test_sms_store.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_api_common_validation_and_rate_limits(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "api_common_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "components" / "api_common" / "include"),
                    str(ROOT / "components" / "api_common" / "api_common.c"),
                    str(ROOT / "tests" / "test_api_common.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


    def test_persistent_idempotency_reservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "api_idempotency_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "tests" / "host_stubs"),
                    "-I", str(ROOT / "components" / "gateway_security" / "include"),
                    "-I", str(ROOT / "components" / "api_idempotency" / "include"),
                    str(ROOT / "components" / "api_idempotency" / "api_idempotency.c"),
                    str(ROOT / "tests" / "test_api_idempotency.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_mqtt_config_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "mqtt_config_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "tests" / "host_stubs"),
                    "-I", str(ROOT / "components" / "gateway_settings" / "include"),
                    str(ROOT / "components" / "gateway_settings" / "mqtt_config.c"),
                    str(ROOT / "tests" / "test_mqtt_config.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_mqtt_command_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "mqtt_command_policy_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "components" / "mqtt_service" / "include"),
                    str(ROOT / "components" / "mqtt_service" / "mqtt_command_policy.c"),
                    str(ROOT / "tests" / "test_mqtt_command_policy.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_mqtt_custom_ca_has_client_lifetime_ownership(self):
        source = (ROOT / "components" / "mqtt_service" / "mqtt_service.c").read_text(encoding="utf-8")
        self.assertIn("static char *s_active_ca_pem;", source)
        self.assertIn("cfg.broker.verification.certificate = next_ca_pem;", source)
        self.assertIn("s_active_ca_pem = next_ca_pem;", source)
        self.assertIn("owned_ca_free(&s_active_ca_pem);", source)
        self.assertNotIn("cfg.broker.verification.certificate = s_config.ca_pem", source)

    def test_mqtt_home_assistant_contract(self):
        source = (ROOT / "components" / "mqtt_service" / "mqtt_service.c").read_text(encoding="utf-8")
        self.assertIn('"%s/device/%s/config"', source)
        self.assertIn('cJSON_AddObjectToObject(root, "o")', source)
        self.assertIn('cJSON_AddObjectToObject(root, "cmps")', source)
        self.assertIn('add_component_common(evt, "event"', source)
        self.assertIn('add_component_common(notify, "notify"', source)
        self.assertIn('topic(t, "sms/send")', source)
        # HA birth messages arrive on <discovery_prefix>/status.
        self.assertIn('"%s/status", config.discovery_prefix', source)

    def test_startup_stack_and_portal_contract(self):
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144", defaults)
        settings = (ROOT / "components" / "gateway_settings" / "gateway_settings.c").read_text(encoding="utf-8")
        mqtt = (ROOT / "components" / "mqtt_service" / "mqtt_service.c").read_text(encoding="utf-8")
        # The ~3.8 KB MQTT config/record must never be a stack local again.
        self.assertNotIn("mqtt_record_t record;", settings)
        self.assertNotIn("gateway_mqtt_config_t config = {0};", mqtt)
        self.assertNotIn("gateway_mqtt_config_t next = {0};", mqtt)
        portal = (ROOT / "components" / "network_service" / "provisioning_portal.c").read_text(encoding="utf-8")
        for route in ('"/api/setup/info"', '"/api/setup/scan"', '"/api/setup/status"', '"/api/setup"'):
            self.assertIn(route, portal)

    def test_usb_routing_and_crash_diagnostics_contract(self):
        board = (ROOT / "components" / "board" / "gateway_board.c").read_text(encoding="utf-8")
        # USB_SEL high = USB_HOST socket (modem), low = USB_DEV (user guide, esp-bsp).
        self.assertIn("set_output(PIN_USB_SEL, 1)", board)
        # MIC2005A FAULT/ is active low.
        self.assertIn("gpio_get_level(PIN_USB_OVERCURRENT) == 0", board)
        partitions = (ROOT / "partitions.csv").read_text(encoding="utf-8")
        self.assertIn("coredump,   data, coredump,", partitions)
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y", defaults)
        self.assertIn("CONFIG_ESP_CONSOLE_SECONDARY_NONE=y", defaults)
        app = (ROOT / "main" / "app_main.c").read_text(encoding="utf-8")
        self.assertIn("gateway_diag_init()", app)
        self.assertNotIn("ESP_ERROR_CHECK(display_service_init())", app)
        self.assertIn("BOOT_STEP(network_service_init(&net_cfg))", app)
        display = (ROOT / "components" / "display_service" / "display_service.c").read_text(encoding="utf-8")
        # A full-frame buffer (115 KB) starved Wi-Fi of internal RAM; draw in strips.
        self.assertIn("LCD_W * BAND_H * sizeof(*s_band)", display)
        self.assertNotIn("LCD_W * LCD_H * sizeof", display)

    def test_openapi_exposes_redacted_mqtt_config(self):
        text = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")
        self.assertIn('/api/v1/config/mqtt:', text)
        self.assertIn('password_set:', text)
        self.assertIn('ca_configured:', text)
        self.assertIn('writeOnly: true', text)

    def test_m61_pre_hardware_hardening_contract(self):
        sdk = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        mqtt = (ROOT / "components" / "mqtt_service" / "mqtt_service.c").read_text(encoding="utf-8")
        usb = (ROOT / "components" / "usb_modem_transport" / "usb_modem_transport.c").read_text(encoding="utf-8")
        board = (ROOT / "components" / "board" / "gateway_board.c").read_text(encoding="utf-8")
        api = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")

        self.assertIn("CONFIG_USB_HOST_HUBS_SUPPORTED=y", sdk)
        self.assertIn("MQTT_EVENT_PUBLISHED", mqtt)
        self.assertIn("replay_cursor_store", mqtt)
        self.assertIn("work->retained", mqtt)
        self.assertIn("s_usb.active_generation", usb)
        self.assertIn("huawei_mode_switch_message", usb)
        self.assertIn("USB host over-current asserted", board)
        self.assertIn("version: 0.7.0", api)
        self.assertIn("/api/v1/system/idempotency/clear-pending:", api)
        self.assertIn('"422":', api)

    def test_at_parser_and_protocol(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "at_parser_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "components" / "at_engine" / "include"),
                    str(ROOT / "components" / "at_engine" / "at_parser.c"),
                    str(ROOT / "components" / "at_engine" / "at_protocol.c"),
                    str(ROOT / "components" / "at_engine" / "at_router.c"),
                    str(ROOT / "tests" / "test_at_parser.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_ota_version_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "ota_version_policy_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(ROOT / "components" / "ota_service" / "include"),
                    str(ROOT / "components" / "ota_service" / "ota_version_policy.c"),
                    str(ROOT / "tests" / "test_ota_version_policy.c"),
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_m7_ota_and_release_contract(self):
        api = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")
        app = (ROOT / "main" / "app_main.c").read_text(encoding="utf-8")
        ota = (ROOT / "components" / "ota_service" / "ota_service.c").read_text(encoding="utf-8")
        sdk = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("version: 0.7.0", api)
        self.assertIn("/api/v1/system/firmware:", api)
        self.assertIn("X-Firmware-SHA256", api)
        self.assertIn("gateway_ota_mark_services_ready", app)
        self.assertNotIn("confirm_ota_image_after_self_test", app)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", ota)
        self.assertIn("esp_ota_set_boot_partition", ota)
        self.assertIn("CONFIG_GATEWAY_OTA_CONFIRM_DELAY_SECONDS=30", sdk)
        self.assertIn('set(PROJECT_VER "0.7.0-dev")', cmake)

    def test_github_workflows_current_and_flashable(self):
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        release = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        for text in (ci, release):
            self.assertIn("actions/checkout@v7", text)
            self.assertIn("actions/setup-python@v7", text)
            self.assertIn("espressif/install-esp-idf-action@v1", text)
            self.assertIn('version: "v6.0.2"', text)
            self.assertIn("inspect_app_image.py", text)
            self.assertIn("merge-bin", text)
            self.assertIn("release_manifest.py", text)
        self.assertIn("actions/upload-artifact@v7", ci)
        self.assertIn("workflow_dispatch:", ci)
        self.assertIn("workflow_dispatch:", release)
        self.assertNotIn("esp-web-tools", ci.lower() + release.lower())

    def test_app_image_inspector_fixture(self):
        import struct
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "app.bin"
            raw = bytearray(32 + 256)
            struct.pack_into("<II", raw, 32, 0xABCD5432, 0)
            raw[32 + 16:32 + 48] = b"0.7.0-dev\0".ljust(32, b"\0")
            raw[32 + 48:32 + 80] = b"esp32_sms_gateway\0".ljust(32, b"\0")
            image.write_bytes(raw)
            subprocess.run(
                [
                    "python3", str(ROOT / "scripts" / "inspect_app_image.py"), str(image),
                    "--expect-project", "esp32_sms_gateway", "--expect-version", "0.7.0-dev",
                ],
                check=True,
            )

    def test_release_manifest_generation(self):
        import hashlib
        import json
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            ota = dist / "gateway-ota.bin"
            factory = dist / "gateway-factory.bin"
            ota.write_bytes(b"ota-image")
            factory.write_bytes(b"factory-image")
            subprocess.run(
                [
                    "python3", str(ROOT / "scripts" / "release_manifest.py"),
                    "--dist", str(dist), "--version", "0.7.0",
                    "--ota", ota.name, "--factory", factory.name,
                    "--git-sha", "0123456789abcdef0123456789abcdef01234567",
                ],
                check=True,
            )
            manifest = json.loads((dist / "release-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["project"], "esp32_sms_gateway")
            self.assertEqual(manifest["version"], "0.7.0")
            self.assertEqual(manifest["source_revision"], "0123456789abcdef0123456789abcdef01234567")
            self.assertEqual(manifest["artifacts"]["factory"]["flash_offset"], "0x0")
            self.assertEqual(manifest["artifacts"]["ota"]["sha256"], hashlib.sha256(b"ota-image").hexdigest())


if __name__ == "__main__":
    unittest.main()


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {signature}")


def _schema_block(spec: str, name: str) -> str:
    lines = spec.splitlines()
    start = lines.index(f"    {name}:")
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if line.startswith("    ") and not line.startswith("     ") and line.strip().endswith(":"):
            end = index
            break
    return "\n".join(lines[start:end])


class OpenApiMatchesCodeTests(unittest.TestCase):
    """Keeps api/openapi.yaml in step with the REST handlers (stdlib only)."""

    def setUp(self):
        self.api = (ROOT / "components" / "api_server" / "api_server.c").read_text(encoding="utf-8")
        self.spec = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")

    def _spec_paths(self):
        paths = {}
        current = None
        for line in self.spec.splitlines():
            if line.startswith("  /api/") and line.endswith(":"):
                current = line.strip()[:-1]
                paths[current] = set()
            elif current and line.startswith("    ") and not line.startswith("     ") and line.strip()[:-1] in {"get", "post", "patch", "delete", "put"}:
                paths[current].add(line.strip()[:-1])
            elif line and not line.startswith(" "):
                current = None
        return paths

    def test_every_registered_route_is_documented(self):
        import re
        paths = self._spec_paths()
        routes = re.findall(r'register_uri\("([^"]+)", HTTP_([A-Z]+)', self.api)
        self.assertGreater(len(routes), 10)
        for uri, method in routes:
            method = method.lower()
            if uri.endswith("/*"):
                base = uri[:-1]
                candidates = [p for p in paths if p.startswith(base + "{")]
            else:
                candidates = [uri] if uri in paths else []
            self.assertTrue(candidates, f"{uri} is not in api/openapi.yaml")
            self.assertTrue(any(method in paths[p] for p in candidates),
                            f"{method.upper()} {uri} is not in api/openapi.yaml")

    def test_status_fields_are_documented(self):
        import re
        body = _function_body(self.api, "static esp_err_t status_handler(")
        schema = _schema_block(self.spec, "GatewayStatus")
        keys = set(re.findall(r'cJSON_Add\w*ToObject\(\s*\w+,\s*"([a-z0-9_]+)"', body))
        keys |= set(re.findall(r'add_json_string\(\s*\w+,\s*"([a-z0-9_]+)"', body))
        self.assertGreater(len(keys), 40)
        missing = sorted(k for k in keys if not re.search(rf"^\s+{k}:", schema, re.M))
        self.assertEqual(missing, [], "status fields missing from GatewayStatus in api/openapi.yaml")

    def test_mqtt_config_fields_are_documented(self):
        import re
        body = _function_body(self.api, "static cJSON *mqtt_config_json(")
        schema = _schema_block(self.spec, "MqttConfig")
        keys = set(re.findall(r'cJSON_Add\w*ToObject\(\s*\w+,\s*"([a-z0-9_]+)"', body))
        keys |= set(re.findall(r'add_json_string\(\s*\w+,\s*"([a-z0-9_]+)"', body))
        missing = sorted(k for k in keys if not re.search(rf"^\s+{k}:", schema, re.M))
        self.assertEqual(missing, [], "MQTT config fields missing from MqttConfig in api/openapi.yaml")


class ChangelogTests(unittest.TestCase):
    def test_release_notes_come_from_changelog(self):
        result = subprocess.run(
            ["python3", str(ROOT / "scripts" / "changelog_section.py"), "v0.7.0-alpha.14"],
            check=True, capture_output=True, text=True,
        )
        self.assertIn("Home Assistant diagnostics", result.stdout)
        self.assertNotIn("## 0.7.0-alpha.13", result.stdout)
        missing = subprocess.run(
            ["python3", str(ROOT / "scripts" / "changelog_section.py"), "9.9.9"],
            capture_output=True, text=True,
        )
        self.assertEqual(missing.returncode, 1)

    def test_every_tag_style_heading_is_well_formed(self):
        import re
        text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        for line in text.splitlines():
            if line.startswith("## ") and line != "## Unreleased":
                self.assertRegex(line, r"^## \d+\.\d+\.\d+(-[0-9A-Za-z.]+)?( to \d+\.\d+\.\d+(-[0-9A-Za-z.]+)?)? - \d{4}-\d{2}-\d{2}$")
