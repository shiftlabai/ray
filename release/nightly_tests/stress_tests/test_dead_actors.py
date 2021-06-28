#!/usr/bin/env python

import json
import logging
import numpy as np
import os
import sys
import time

import ray

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

ray.init(address="auto")

# These numbers need to correspond with the autoscaler config file.
# The number of remote nodes in the autoscaler should upper bound
# these because sometimes nodes fail to update.
NUM_REMOTE_NODES = 100
HEAD_NODE_CPUS = 2
NUM_REMOTE_CPU = NUM_REMOTE_NODES * HEAD_NODE_CPUS


@ray.remote
class Child(object):
    def __init__(self, death_probability):
        self.death_probability = death_probability

    def ping(self):
        # Exit process with some probability.
        exit_chance = np.random.rand()
        if exit_chance > self.death_probability:
            sys.exit(-1)


@ray.remote
class Parent(object):
    def __init__(self, num_children, death_probability):
        self.death_probability = death_probability
        self.children = [
            Child.remote(death_probability) for _ in range(num_children)
        ]

    def ping(self, num_pings):
        children_outputs = []
        for _ in range(num_pings):
            children_outputs += [
                child.ping.remote() for child in self.children
            ]
        try:
            ray.get(children_outputs)
        except Exception:
            # Replace the children if one of them died.
            self.__init__(len(self.children), self.death_probability)

    def kill(self):
        # Clean up children.
        ray.get([child.__ray_terminate__.remote() for child in self.children])


if __name__ == "__main__":
    result = {"success": 0}
    try:
        # Wait until the expected number of nodes have joined the cluster.
        while True:
            num_nodes = len(ray.nodes())
            logger.info("Waiting for nodes {}/{}".format(
                num_nodes, NUM_REMOTE_NODES + 1))
            if num_nodes >= NUM_REMOTE_NODES + 1:
                break
            time.sleep(5)
        logger.info("Nodes have all joined. There are %s resources.",
                    ray.cluster_resources())

        num_parents = 10
        num_children = 10
        death_probability = 0.95

        parents = [
            Parent.remote(num_children, death_probability)
            for _ in range(num_parents)
        ]

        start = time.time()
        loop_times = []
        for i in range(100):
            loop_start = time.time()
            ray.get([parent.ping.remote(10) for parent in parents])

            # Kill a parent actor with some probability.
            exit_chance = np.random.rand()
            if exit_chance > death_probability:
                parent_index = np.random.randint(len(parents))
                parents[parent_index].kill.remote()
                parents[parent_index] = Parent.remote(num_children,
                                                      death_probability)

            logger.info("Finished trial %s", i)
            loop_times.append(time.time() - loop_start)

        print("Finished in: {}s".format(time.time() - start))
        print("Average iteration time: {}s".format(
            sum(loop_times) / len(loop_times)))
        print("Max iteration time: {}s".format(max(loop_times)))
        print("Min iteration time: {}s".format(min(loop_times)))
        result["total_time"] = time.time() - start
        result["avg_iteration_time"] = sum(loop_times) / len(loop_times)
        result["max_iteration_time"] = max(loop_times)
        result["min_iteration_time"] = min(loop_times)
        result["success"] = 1
        print("PASSED.")
    except Exception as e:
        logging.exception(e)
        print("FAILED.")
    with open(os.environ["TEST_OUTPUT_JSON"], "w") as f:
        f.write(json.dumps(result))