/*
 * Copyright 2013  QNX Software Systems Limited
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0.
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the applicable QNX License Guide at
 * http://www.qnx.com/legal/licensing/document_archive/current_matrix.pdf
 * for other information.
 */

/**
 * The abstraction layer for Bluetooth functionality
 */

var _pps = qnx.webplatform.pps,
	_pairedDevicesPPS,
	_controlPPS,
	_statusPPS,

	_pairedDevices = {};

/* TODO Please make sure that constants below are identical to ones in client.js*/

/** Defines All allowed Profiles ID for current device*/
var SERVICE_ALL = "ALL";


/* TODO Please make sure that constants above are identical to ones in client.js*/

/**
 * Method called when object changes in paired_devices directory (new paired device available)
 * @param event {Object} The PPS event
 */
function onPairedDevice(event) {
	if (event && event.data && event.data.cod && event.data.name && event.data.paired != undefined && event.data.rssi) {
		var device = {
			mac:event.objName,
			cod:event.data.cod.replace(/(\r\n|\n|\r)/gm, ""), // TODO temp fix of the bug with garbage in JSON generated by PPS-Bluetooth, remove it when PPS-Bluetooth will be fixed
			name:event.data.name,
			paired:event.data.paired,
			rssi:event.data.rssi.replace(/(\r\n|\n|\r)/gm, "") // TODO temp fix of the bug with garbage in JSON generated by PPS-Bluetooth, remove it when PPS-Bluetooth will be fixed
		};

		// saving paired device in the list to maintain local copy
		_pairedDevices[device.mac] = device;
	}
}

/**
 * onChange event handler for status PPS object
 * convenience function to reduce size of init(), not to invoke from outside
 * @param event {Object} onChange event object containing event data from PPS
 * */
function onStatusPPSChange(event) {
	/* checking if all required field are present when event generated*/
	if (event && event.data && event.data.event) {
		var mac = event.data.data;
		switch (event.data.event) {
			/* Event indicated that one specified paired device deleted */
			case "BTMGR_EVENT_DEVICE_DELETED":
				// deleting device from local list of devices when it was deleted by pps-bluetooth from the list of paired devices
				delete _pairedDevices[mac];
				break;
		}
	}
}


/**
 * Exports are the publicly accessible functions
 */
module.exports = {

	/**
	 * Initializes the extension,
	 * open and initialise required PPS object and event handlers
	 */
	init:function () {

		/* Initialise PPS object which populated when user Paired with device */
		_pairedDevicesPPS = _pps.createObject("/pps/services/bluetooth/paired_devices/.all", _pps.PPSMode.DELTA);

		/* We have to monitor onFirstReadComplete and onNewData event to capture all devices */
		_pairedDevicesPPS.onFirstReadComplete = onPairedDevice;
		_pairedDevicesPPS.onNewData = onPairedDevice;

		_pairedDevicesPPS.open(_pps.FileMode.RDONLY);

		/* Initialise PPS object to send commands and data to the PPS-Bluetooth */
		_controlPPS = _pps.createObject("/pps/services/bluetooth/control", _pps.PPSMode.DELTA);
		_controlPPS.open(_pps.FileMode.WRONLY);

		/* Initialise PPS object responsible for notifying about BluettothStake state changes */
		_statusPPS = _pps.createObject("/pps/services/bluetooth/status", _pps.PPSMode.DELTA);
		_statusPPS.onNewData = onStatusPPSChange;
		_statusPPS.open(_pps.FileMode.RDONLY);
	},

	/**
	 * Connects to specified service on device with specified MAC address
	 * @param service {String} Service identifier
	 * @param mac {String} MAC address of the device
	 */
	connectService:function (service, mac) {
		if (service && mac) {
			if (service == SERVICE_ALL) {
				_controlPPS.write({
					"command":"connect_all",
					"data":mac
				});
			} else {
				_controlPPS.write({
					"command":"connect_service",
					"data":mac,
					"data2":service
				});
			}
		}
	},

	/**
	 * Return list of paired devices.
	 *
	 * TODO This has to be changed when JNEXT extension would be able to retrieve values from .all
	 * right now I have to keep track of it on extension side.
	 *
	 * @returns {Object} The list of currently paired devices
	 */
	getPaired:function () {
		return _pairedDevices;
	},
};