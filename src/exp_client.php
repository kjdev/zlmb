<?php
/*
 * Example client
 *
 * Usage: exp_client.php ENDPOINT MESSAGE ...
 *
 * % php exp_client.php tcp://127.0.0.1:5557 message
 * % php exp_client.php tcp://127.0.0.1:5557 message1 message2 message3
 */

if (!extension_loaded('zmq')) {
    echo "Required extension to zmq\n";
    exit(1);
}

if ($argc <= 2) {
    echo "Usage: ", basename(__FILE__), " ENDPOINT MESSAGE ...\n";
    exit(1);
}

$endpoint = $argv[1];

try {
    $context = new ZMQContext();
    $socket = $context->getSocket(ZMQ::SOCKET_PUSH);
    $socket->connect($endpoint);

    $last = $argc - 1;
    for ($i = 2; $i != $last; $i++) {
        $socket->send($argv[$i], ZMQ::MODE_SNDMORE);
    }
    $socket->send($argv[$last]);
} catch (Exception $e) {
    echo "ERR:", $e->getMessage(), "\n";
}
