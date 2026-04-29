#!/usr/bin/env python3
"""
ONVIF Analytics Service CGI — Profile M
Exposes the dashcam's DMS/ADAS AI detection modules as ONVIF analytics.

AI modules:
  DMS (Driver Monitoring):
    - MobilePhoneDetection    — smartphone usage detection
    - SeatbeltDetection       — seatbelt compliance check
    - DrowsinessDetection     — driver drowsiness / fatigue
    - DriverIdentification    — facial recognition for driver ID

  ADAS (Advanced Driver Assistance):
    - TailgatingDetection     — following distance monitor
    - ANPRDetection           — automatic number plate recognition
    - CrashDetection          — collision / impact detection (IMU)
"""

import os
import sys
import json
import glob
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

NS_SOAP = 'http://www.w3.org/2003/05/soap-envelope'
NS_TAN  = 'http://www.onvif.org/ver20/analytics/wsdl'
NS_TT   = 'http://www.onvif.org/ver10/schema'

AI_DIR = os.environ.get('AI_DIR', '/mnt/videodata/ai')

# Analytics module definitions
ANALYTICS_MODULES = [
    {
        'token': 'MobilePhoneDetection',
        'name': 'Mobile Phone Detection',
        'type': 'tt:CellMotionDetector',
        'description': 'Detects smartphone usage by driver (DMS)',
        'ai_subdir': 'mobile',
    },
    {
        'token': 'SeatbeltDetection',
        'name': 'Seatbelt Detection',
        'type': 'tt:CellMotionDetector',
        'description': 'Detects seatbelt compliance (DMS)',
        'ai_subdir': 'seatbelt',
    },
    {
        'token': 'DrowsinessDetection',
        'name': 'Drowsiness Detection',
        'type': 'tt:CellMotionDetector',
        'description': 'Detects driver drowsiness and fatigue (DMS)',
        'ai_subdir': 'drowsiness',
    },
    {
        'token': 'DriverIdentification',
        'name': 'Driver Identification',
        'type': 'tt:CellMotionDetector',
        'description': 'Identifies driver via facial recognition (DMS)',
        'ai_subdir': 'driver_id',
    },
    {
        'token': 'TailgatingDetection',
        'name': 'Tailgating Detection',
        'type': 'tt:CellMotionDetector',
        'description': 'Monitors following distance to vehicle ahead (ADAS)',
        'ai_subdir': 'tailgating',
    },
    {
        'token': 'ANPRDetection',
        'name': 'License Plate Recognition',
        'type': 'tt:CellMotionDetector',
        'description': 'Automatic number plate recognition (ADAS)',
        'ai_subdir': 'anpr',
    },
    {
        'token': 'CrashDetection',
        'name': 'Crash Detection',
        'type': 'tt:CellMotionDetector',
        'description': 'Collision and impact detection via IMU (ADAS)',
        'ai_subdir': 'crash',
    },
]


def epoch_to_onvif(epoch):
    dt = datetime.fromtimestamp(int(epoch), tz=timezone.utc)
    return dt.strftime('%Y-%m-%dT%H:%M:%SZ')


def soap_response(body_xml):
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"'
        ' xmlns:tan="http://www.onvif.org/ver20/analytics/wsdl"'
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


def handle_get_service_capabilities():
    soap_response(
        '<tan:GetServiceCapabilitiesResponse>'
        '<tan:Capabilities RuleSupport="true"'
        ' AnalyticsModuleSupport="true"'
        ' CellBasedSceneDescriptionSupported="false">'
        '</tan:Capabilities>'
        '</tan:GetServiceCapabilitiesResponse>'
    )


def handle_get_analytics_engines():
    """Return the two analytics engines: DMS and ADAS."""
    soap_response(
        '<tan:GetAnalyticsEnginesResponse>'
        '<tan:AnalyticsEngine token="DMS_Engine">'
        '<tt:Name>Driver Monitoring System</tt:Name>'
        '<tt:UseCount>1</tt:UseCount>'
        '</tan:AnalyticsEngine>'
        '<tan:AnalyticsEngine token="ADAS_Engine">'
        '<tt:Name>Advanced Driver Assistance</tt:Name>'
        '<tt:UseCount>1</tt:UseCount>'
        '</tan:AnalyticsEngine>'
        '</tan:GetAnalyticsEnginesResponse>'
    )


def handle_get_analytics_modules(root):
    """Return configured analytics modules for a configuration token."""
    items = []
    for mod in ANALYTICS_MODULES:
        items.append(
            f'<tan:AnalyticsModule token="{mod["token"]}">'
            f'<tt:Name>{mod["name"]}</tt:Name>'
            f'<tt:Type>{mod["type"]}</tt:Type>'
            '<tt:Parameters>'
            f'<tt:SimpleItem Name="Enabled" Value="true"/>'
            f'<tt:SimpleItem Name="Description" Value="{mod["description"]}"/>'
            f'<tt:SimpleItem Name="AIDirectory" Value="{AI_DIR}/{mod["ai_subdir"]}"/>'
            '</tt:Parameters>'
            f'</tan:AnalyticsModule>'
        )

    soap_response(
        '<tan:GetAnalyticsModulesResponse>'
        + ''.join(items) +
        '</tan:GetAnalyticsModulesResponse>'
    )


def handle_get_supported_analytics_modules():
    """Return the types of analytics modules we support."""
    items = []
    for mod in ANALYTICS_MODULES:
        items.append(
            '<tan:SupportedAnalyticsModule>'
            f'<tt:Type>{mod["type"]}</tt:Type>'
            f'<tt:Name>{mod["name"]}</tt:Name>'
            '<tt:Parameters>'
            f'<tt:SimpleItem Name="Token" Value="{mod["token"]}"/>'
            f'<tt:SimpleItem Name="Category" '
            f'Value="{"DMS" if mod["token"] in ("MobilePhoneDetection","SeatbeltDetection","DrowsinessDetection","DriverIdentification") else "ADAS"}"/>'
            '</tt:Parameters>'
            '</tan:SupportedAnalyticsModule>'
        )

    soap_response(
        '<tan:GetSupportedAnalyticsModulesResponse>'
        + ''.join(items) +
        '</tan:GetSupportedAnalyticsModulesResponse>'
    )


def handle_get_analytics_engine_inputs():
    """Which video sources feed into analytics."""
    soap_response(
        '<tan:GetAnalyticsEngineInputsResponse>'
        '<tan:AnalyticsEngineInput token="DMS_Input">'
        '<tt:SourceToken>VideoSource_inside</tt:SourceToken>'
        '<tt:Name>Interior Camera (DMS)</tt:Name>'
        '</tan:AnalyticsEngineInput>'
        '<tan:AnalyticsEngineInput token="ADAS_Input">'
        '<tt:SourceToken>VideoSource_front</tt:SourceToken>'
        '<tt:Name>Front Camera (ADAS)</tt:Name>'
        '</tan:AnalyticsEngineInput>'
        '</tan:GetAnalyticsEngineInputsResponse>'
    )


def handle_get_rules(root):
    """Return analytics rules (detection triggers)."""
    items = []
    for mod in ANALYTICS_MODULES:
        items.append(
            f'<tan:Rule token="Rule_{mod["token"]}">'
            f'<tt:Name>{mod["name"]} Rule</tt:Name>'
            f'<tt:Type>{mod["type"]}</tt:Type>'
            '<tt:Parameters>'
            '<tt:SimpleItem Name="Sensitivity" Value="50"/>'
            '</tt:Parameters>'
            f'</tan:Rule>'
        )

    soap_response(
        '<tan:GetRulesResponse>'
        + ''.join(items) +
        '</tan:GetRulesResponse>'
    )


def handle_get_supported_rules():
    """Return supported rule types."""
    soap_response(
        '<tan:GetSupportedRulesResponse>'
        '<tan:SupportedRules>'
        '<tt:RuleDescription Name="CellMotionDetector">'
        '<tt:Parameters>'
        '<tt:SimpleItemDescription Name="Sensitivity"'
        ' Type="xs:integer"/>'
        '</tt:Parameters>'
        '</tt:RuleDescription>'
        '</tan:SupportedRules>'
        '</tan:GetSupportedRulesResponse>'
    )


def handle_get_analytics_state():
    """Return current state of each analytics module with recent detections."""
    items = []
    for mod in ANALYTICS_MODULES:
        subdir = os.path.join(AI_DIR, mod['ai_subdir'])
        count = 0
        latest = ''
        if os.path.isdir(subdir):
            files = sorted(glob.glob(os.path.join(subdir, '*')))
            count = len(files)
            if files:
                latest = epoch_to_onvif(os.path.getmtime(files[-1]))

        items.append(
            f'<tan:AnalyticsState token="{mod["token"]}">'
            f'<tt:Name>{mod["name"]}</tt:Name>'
            '<tt:Parameters>'
            f'<tt:SimpleItem Name="Active" Value="true"/>'
            f'<tt:SimpleItem Name="DetectionCount" Value="{count}"/>'
            f'<tt:SimpleItem Name="LastDetection" Value="{latest}"/>'
            '</tt:Parameters>'
            f'</tan:AnalyticsState>'
        )

    soap_response(
        '<tan:GetAnalyticsStateResponse>'
        + ''.join(items) +
        '</tan:GetAnalyticsStateResponse>'
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

    soap_body = root.find(f'{{{NS_SOAP}}}Body')
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
        'GetServiceCapabilities':        handle_get_service_capabilities,
        'GetAnalyticsEngines':           handle_get_analytics_engines,
        'GetAnalyticsModules':           lambda: handle_get_analytics_modules(root),
        'GetSupportedAnalyticsModules':  handle_get_supported_analytics_modules,
        'GetAnalyticsEngineInputs':      handle_get_analytics_engine_inputs,
        'GetRules':                      lambda: handle_get_rules(root),
        'GetSupportedRules':             handle_get_supported_rules,
        'GetAnalyticsState':             handle_get_analytics_state,
    }

    handler = handlers.get(tag)
    if handler:
        handler()
    else:
        soap_fault('s:Sender', f'Action not supported: {tag}')


if __name__ == '__main__':
    main()
