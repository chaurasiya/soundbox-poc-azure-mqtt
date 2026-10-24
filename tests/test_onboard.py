import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import onboard_device as o


class OnboardTests(unittest.TestCase):
    def test_imei_validation(self):
        self.assertTrue(o.valid_imei("356938035643809"))      # standard Luhn-valid example IMEI
        self.assertFalse(o.valid_imei("356938035643808"))     # bad check digit
        self.assertFalse(o.valid_imei("35693803564380"))      # 14 digits
        self.assertFalse(o.valid_imei("35693803564380a"))
        self.assertFalse(o.valid_imei("../etc/passwd0000"))   # no path tricks reach the filesystem

    def test_host_parsing(self):
        self.assertEqual(o.parse_host("rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net"),
                         ("rupeeco-pawan-mqtt", "centralindia"))
        self.assertIsNone(o.parse_host("evil.example.com"))
        self.assertIsNone(o.parse_host("x.centralindia-1.ts.eventgrid.azure.net.evil.com"))

    def test_client_body(self):
        b = o.client_body("356938035643809", "AB" * 20)["properties"]
        self.assertEqual(b["authenticationName"], "356938035643809")
        self.assertEqual(b["attributes"]["type"], "soundbox")
        self.assertEqual(b["clientCertificateAuthentication"]["allowedThumbprints"], ["AB" * 20])


if __name__ == "__main__":
    unittest.main()
