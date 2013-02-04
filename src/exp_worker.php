<?php
/*
 * Example worker
 *
 * Usage: exp_worker.php ENDPOINT MESSAGE ...
 *
 * % php exp_worker.php tcp://127.0.0.1:5560
 */

if (!extension_loaded('zmq')) {
    echo "Required extension to zmq\n";
    exit(1);
}

if ($argc <= 1) {
    echo "Usage: ", basename(__FILE__), " ENDPOINT\n";
    exit(1);
}

$endpoint = $argv[1];

try {
    $context = new ZMQContext();
    $socket = $context->getSocket(ZMQ::SOCKET_PULL);
    $socket->connect($endpoint);

    while (1) {
        $msg = array();
        do {
            array_push($msg, $socket->recv());
        } while ($socket->getSockOpt(ZMQ::SOCKOPT_RCVMORE));
        var_dump($msg);
    }
} catch (Exception $e) {
    echo "ERR:", $e->getMessage(), "\n";
}
