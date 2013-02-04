#!/bin/env php
<?php
/*
 * Example worker exec
 *
 * Usage: exp_worker_exec.php
 *
 * zlmb-worker -e tcp://127.0.0.1:5560 -c exp_worker_exec.php
 */

//Init zlmb
$zlmb_frame = 0;
$zlmb_frame_length = 0;
$zlmb_length = 0;
$zlmb_buffer = '';

//Read STDIN (Get zlmb buffer)
$fp = fopen('php://stdin', 'r');
if ($fp) {
    stream_set_blocking($fp, false);
    while (!feof($fp)) {
        $read = fread($fp, 1024);
        if ($read === FALSE || empty($read)) {
            break;
        }
        $zlmb_buffer .= $read;
    }
    fclose($fp);
}

//Get zlmb environ
if (isset($_SERVER['ZLMB_FRAME'])) {
    $zlmb_frame = $_SERVER['ZLMB_FRAME'];
}
if (isset($_SERVER['ZLMB_FRAME_LENGTH'])) {
    $zlmb_frame_length = $_SERVER['ZLMB_FRAME_LENGTH'];
}
if (isset($_SERVER['ZLMB_LENGTH'])) {
    $zlmb_length = $_SERVER['ZLMB_LENGTH'];
}

//Output
echo basename(__FILE__), ": ", date('Y-m-d H:i:s'), "\n";
echo "ZLMB_FRAME:$zlmb_frame\n";
echo "ZLMB_FRAME_LENGTH:$zlmb_frame_length\n";
echo "ZLMB_LENGTH:$zlmb_length\n";
echo "ZLMB_BUFFER:$zlmb_buffer\n";

//Parse zlmb frame buffer
$zlmb_frames = array();
if (!empty($zlmb_frame_length)) {
    $start = 0;
    $len = explode(':', $zlmb_frame_length);
    foreach ($len as $val) {
        $buf = substr($zlmb_buffer, $start, $val);
        $start += $val;
        array_push($zlmb_frames, $buf);
    }
}

var_dump($zlmb_frames);

echo "----------\n";
