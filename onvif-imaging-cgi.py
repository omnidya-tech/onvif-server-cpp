#!/usr/bin/env python3
"""
ONVIF Imaging Service CGI — Profile T
Controls camera imaging parameters via the VeriSilicon ISP on IMX8MP.

Uses the isp_ctrl helper to send JSON commands to isp_media_server
through the viv_ext_ctrl V4L2 extended control.

ISP commands used:
  cproc.g.cfg / cproc.s.cfg  — brightness, contrast, saturation, hue
  ec.g.cfg / ec.s.cfg        — exposure gain and integration time
  awb.g.cfg / awb.s.cfg      — white balance gains
  ae.g.cfg / ae.s.cfg        — auto-exposure enable/mode
"""

import os
import sys
import json
import subprocess
import xml.etree.ElementTree as ET

NS = {
    's':    'http://www.w3.org/2003/05/soap-envelope',
    'timg': 'http://www.onvif.org/ver20/imaging/wsdl',
    'tt':   'http://www.onvif.org/ver10/schema',
}

VIDEO_DEV = os.environ.get('IMAGING_VIDEO_DEV', '/dev/video2')
ISP_CTRL = os.environ.get('ISP_CTRL', '/usr/bin/isp_ctrl')
STREAM_ID = int(os.environ.get('ISP_STREAM_ID', '0'))


def isp_cmd(cmd_dict):
    """Send a JSON command to the ISP and return the parsed response."""
    cmd_dict.setdefault('streamid', STREAM_ID)
    try:
        out = subprocess.check_output(
            [ISP_CTRL, VIDEO_DEV, json.dumps(cmd_dict)],
            stderr=subprocess.DEVNULL, timeout=5
        ).decode('utf-8', errors='replace').strip()
        return json.loads(out) if out else {}
    except Exception:
        return {}


def isp_get_cproc():
    """Read CPROC (color processing) settings."""
    return isp_cmd({'id': 'cproc.g.cfg'})


def isp_set_cproc(**kwargs):
    """Set CPROC parameters (brightness, contrast, saturation, hue)."""
    cmd = {'id': 'cproc.s.cfg'}
    cmd.update(kwargs)
    return isp_cmd(cmd)


def isp_get_exposure():
    """Read exposure/gain settings."""
    return isp_cmd({'id': 'ec.g.cfg'})


def isp_set_exposure(**kwargs):
    """Set exposure parameters (gain, time)."""
    cmd = {'id': 'ec.s.cfg'}
    cmd.update(kwargs)
    return isp_cmd(cmd)


def isp_get_ae():
    """Read auto-exposure config."""
    return isp_cmd({'id': 'ae.g.cfg'})


def isp_get_awb():
    """Read auto white balance config."""
    return isp_cmd({'id': 'awb.g.cfg'})


def soap_response(body_xml):
    """Wrap body XML in a SOAP envelope and emit CGI response."""
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"'
        ' xmlns:timg="http://www.onvif.org/ver20/imaging/wsdl"'
        ' xmlns:tt="http://www.onvif.org/ver10/schema">'
        '<s:Body>' + body_xml + '</s:Body>'
        '</s:Envelope>'
    )
    sys.stdout.write('Content-Type: application/soap+xml; charset=utf-8\r\n')
    sys.stdout.write(f'Content-Length: {len(xml)}\r\n')
    sys.stdout.write('\r\n')
    sys.stdout.write(xml)


def soap_fault(code, reason):
    soap_response(
        f'<s:Fault><s:Code><s:Value>{code}</s:Value></s:Code>'
        f'<s:Reason><s:Text xml:lang="en">{reason}</s:Text></s:Reason>'
        f'</s:Fault>'
    )


def handle_get_imaging_settings():
    """GetImagingSettings — read current camera parameters from VIV ISP."""
    cproc = isp_get_cproc()
    ec = isp_get_exposure()
    ae = isp_get_ae()

    # CPROC values (VIV ISP ranges)
    brightness = cproc.get('brightness', 0)        # -128 to 127
    contrast = cproc.get('contrast', 1.0)           # 0.0 to 1.99
    saturation = cproc.get('saturation', 1.0)       # 0.0 to 1.99
    hue = cproc.get('hue', 0.0)                     # -90 to 89

    # Map VIV brightness (-128..127) to ONVIF (0..255)
    onvif_brightness = int(brightness) + 128
    # Map VIV contrast/saturation (0..1.99) to ONVIF (0..255)
    onvif_contrast = int(float(contrast) * 128)
    onvif_saturation = int(float(saturation) * 128)
    # Sharpness — not directly available in CPROC, report 128 (neutral)
    onvif_sharpness = 128

    # Exposure
    gain = ec.get('gain', 0)
    exp_time = ec.get('time', 0)
    ae_enabled = ae.get('enable', True)
    exp_mode = 'AUTO' if ae_enabled else 'MANUAL'

    body = (
        '<timg:GetImagingSettingsResponse>'
        '<timg:ImagingSettings>'
        f'<tt:Brightness>{onvif_brightness}</tt:Brightness>'
        f'<tt:ColorSaturation>{onvif_saturation}</tt:ColorSaturation>'
        f'<tt:Contrast>{onvif_contrast}</tt:Contrast>'
        f'<tt:Sharpness>{onvif_sharpness}</tt:Sharpness>'
        '<tt:Exposure>'
        f'<tt:Mode>{exp_mode}</tt:Mode>'
        f'<tt:ExposureTime>{exp_time}</tt:ExposureTime>'
        f'<tt:Gain>{gain}</tt:Gain>'
        '</tt:Exposure>'
        '<tt:WhiteBalance>'
        '<tt:Mode>AUTO</tt:Mode>'
        '</tt:WhiteBalance>'
        '<tt:Extension>'
        f'<tt:SimpleItem Name="VIV_Brightness" Value="{brightness}"/>'
        f'<tt:SimpleItem Name="VIV_Contrast" Value="{contrast}"/>'
        f'<tt:SimpleItem Name="VIV_Saturation" Value="{saturation}"/>'
        f'<tt:SimpleItem Name="VIV_Hue" Value="{hue}"/>'
        '</tt:Extension>'
        '</timg:ImagingSettings>'
        '</timg:GetImagingSettingsResponse>'
    )
    soap_response(body)


def handle_set_imaging_settings(root):
    """SetImagingSettings — apply imaging parameters via VIV ISP."""
    settings = root.find('.//{http://www.onvif.org/ver20/imaging/wsdl}ImagingSettings')
    if settings is None:
        soap_fault('s:Receiver', 'Missing ImagingSettings element')
        return

    # CPROC updates
    cproc_args = {}

    el = settings.find('{http://www.onvif.org/ver10/schema}Brightness')
    if el is not None and el.text:
        # ONVIF 0..255 -> VIV -128..127
        val = int(float(el.text)) - 128
        cproc_args['brightness'] = max(-128, min(127, val))

    el = settings.find('{http://www.onvif.org/ver10/schema}Contrast')
    if el is not None and el.text:
        # ONVIF 0..255 -> VIV 0.0..1.99
        val = float(el.text) / 128.0
        cproc_args['contrast'] = round(max(0.0, min(1.99, val)), 3)

    el = settings.find('{http://www.onvif.org/ver10/schema}ColorSaturation')
    if el is not None and el.text:
        # ONVIF 0..255 -> VIV 0.0..1.99
        val = float(el.text) / 128.0
        cproc_args['saturation'] = round(max(0.0, min(1.99, val)), 3)

    if cproc_args:
        isp_set_cproc(**cproc_args)

    # Exposure updates
    exp = settings.find('{http://www.onvif.org/ver10/schema}Exposure')
    if exp is not None:
        mode_el = exp.find('{http://www.onvif.org/ver10/schema}Mode')
        if mode_el is not None and mode_el.text:
            enable = mode_el.text.upper() == 'AUTO'
            isp_cmd({'id': 'ae.s.en', 'enable': enable})

        ec_args = {}
        time_el = exp.find('{http://www.onvif.org/ver10/schema}ExposureTime')
        if time_el is not None and time_el.text:
            ec_args['time'] = float(time_el.text)
        gain_el = exp.find('{http://www.onvif.org/ver10/schema}Gain')
        if gain_el is not None and gain_el.text:
            ec_args['gain'] = float(gain_el.text)
        if ec_args:
            isp_set_exposure(**ec_args)

    soap_response('<timg:SetImagingSettingsResponse/>')


def handle_get_options():
    """GetOptions — return supported parameter ranges."""
    ec = isp_get_exposure()
    gain_min = ec.get('gain.min', 1.0)
    gain_max = ec.get('gain.max', 16.0)
    time_min = ec.get('inte.min', 100)
    time_max = ec.get('inte.max', 40000)

    body = (
        '<timg:GetOptionsResponse>'
        '<timg:ImagingOptions>'
        '<tt:Brightness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Brightness>'
        '<tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:ColorSaturation>'
        '<tt:Contrast><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Contrast>'
        '<tt:Sharpness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Sharpness>'
        '<tt:Exposure>'
        '<tt:Mode><tt:Item>AUTO</tt:Item><tt:Item>MANUAL</tt:Item></tt:Mode>'
        f'<tt:MinExposureTime>{time_min}</tt:MinExposureTime>'
        f'<tt:MaxExposureTime>{time_max}</tt:MaxExposureTime>'
        f'<tt:MinGain>{gain_min}</tt:MinGain>'
        f'<tt:MaxGain>{gain_max}</tt:MaxGain>'
        '</tt:Exposure>'
        '<tt:WhiteBalance>'
        '<tt:Mode><tt:Item>AUTO</tt:Item><tt:Item>MANUAL</tt:Item></tt:Mode>'
        '</tt:WhiteBalance>'
        '</timg:ImagingOptions>'
        '</timg:GetOptionsResponse>'
    )
    soap_response(body)


def handle_get_move_options():
    """GetMoveOptions — fixed-focus camera, no move options."""
    soap_response('<timg:GetMoveOptionsResponse/>')


def handle_get_status():
    """GetStatus — return current focus/exposure status."""
    soap_response(
        '<timg:GetStatusResponse>'
        '<timg:Status>'
        '<tt:FocusStatus20>'
        '<tt:Position>0</tt:Position>'
        '<tt:MoveStatus>IDLE</tt:MoveStatus>'
        '</tt:FocusStatus20>'
        '</timg:Status>'
        '</timg:GetStatusResponse>'
    )


def main():
    content_length = int(os.environ.get('CONTENT_LENGTH', 0))
    if content_length == 0:
        soap_fault('s:Sender', 'Empty request')
        return

    body = sys.stdin.buffer.read(content_length)

    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        soap_fault('s:Sender', 'Malformed XML')
        return

    soap_body = root.find('{http://www.w3.org/2003/05/soap-envelope}Body')
    if soap_body is None:
        soap_fault('s:Sender', 'Missing SOAP Body')
        return

    action_el = None
    for child in soap_body:
        action_el = child
        break

    if action_el is None:
        soap_fault('s:Sender', 'Empty SOAP Body')
        return

    tag = action_el.tag.split('}')[-1] if '}' in action_el.tag else action_el.tag

    handlers = {
        'GetImagingSettings':  handle_get_imaging_settings,
        'SetImagingSettings':  lambda: handle_set_imaging_settings(root),
        'GetOptions':          handle_get_options,
        'GetMoveOptions':      handle_get_move_options,
        'GetStatus':           handle_get_status,
        'Move':                lambda: soap_response('<timg:MoveResponse/>'),
        'Stop':                lambda: soap_response('<timg:StopResponse/>'),
    }

    handler = handlers.get(tag)
    if handler:
        handler()
    else:
        soap_fault('s:Sender', f'Action not supported: {tag}')


if __name__ == '__main__':
    main()
