import importlib.util
from pathlib import Path
import unittest
import sys
sys.path.insert(0,str(Path(__file__).parents[1]))
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("route", Path(__file__).parents[1] / "route.py")
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


class RoutingTests(unittest.TestCase):
    @patch.object(route.subprocess, "check_output")
    def test_cli_graph_preserves_names_and_link_direction(self, read):
        read.side_effect = ["teedsp:input_FL\n", "shairport-sync:output_FL\n",
            "shairport-sync:output_FL\n  |-> teedsp:input_FL\nteedsp:input_FL\n  |<- shairport-sync:output_FL\n"]
        ports, links = route.graph()
        self.assertEqual(ports[("teedsp", "input_FL")], "teedsp:input_FL")
        self.assertEqual(links, {("shairport-sync:output_FL", "teedsp:input_FL")})

    def setUp(self):
        self.ports = {}
        for index, channel in enumerate(("FL", "FR")):
            for offset, node, port in ((1,route.SOURCE,"output_"), (3,route.SINK,"playback_"),
                                      (5,"teedsp","input_"), (7,"teedsp","output_")):
                self.ports[(node,port+channel)] = offset+index

    @patch.object(route.subprocess, "run")
    @patch.object(route.subprocess, "check_output")
    def test_gain_targets_only_airplay(self, read, run):
        read.return_value = ('id 10, type PipeWire:Interface:Node/3\n'
            '    node.name = "havoice"\n'
            'id 20, type PipeWire:Interface:Node/3\n'
            f'    node.name = "{route.SOURCE}"\n')
        route.source_gain(0.25)
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0],
            ['pw-cli', 'set-param', '20', 'Props', '{"channelVolumes": [0.25, 0.25]}'])

    @patch.object(route.Path, "read_text", return_value='{"volume":50,"muted":false}')
    @patch.object(route, "source_gain")
    @patch.object(route.subprocess, "run")
    def test_restore_attenuates_before_direct_link(self, run, gain, read):
        events = []
        gain.side_effect = lambda value: events.append(('gain', value))
        run.side_effect = lambda *a, **kw: events.append(('link', a[0]))
        route.apply(self.ports, {(7,3),(8,4),(1,5),(2,6)}, restore=True)
        self.assertEqual(events[0][0], 'gain')
        self.assertAlmostEqual(events[0][1], 0.3162277, places=5)
        self.assertEqual(events[1][1], ['pw-link', '1', '3'])

    def test_insert_preserves_other_streams(self):
        add, remove = route.plan(self.ports, {(1,3),(2,4),(100,3),(101,4)})
        self.assertEqual(set(add), {(7,3),(8,4),(1,5),(2,6)})
        self.assertEqual(set(remove), {(1,3),(2,4)})

    def test_already_routed_is_noop(self):
        self.assertEqual(route.plan(self.ports, {(7,3),(8,4),(1,5),(2,6)}), ([],[]))

    def test_restore_after_process_crash(self):
        ports = {key:value for key,value in self.ports.items() if key[0] != "teedsp"}
        self.assertEqual(set(route.plan(ports, set(), True)[0]), {(1,3),(2,4)})

    def test_missing_device_is_noop(self):
        ports = {key:value for key,value in self.ports.items() if key[0] != route.SINK}
        self.assertEqual(route.plan(ports, set()), ([],[]))


if __name__ == "__main__":
    unittest.main()
