"""Read-only native snapshot checks usable with the sample or a larger game."""

import math
import time


def check_snapshot_queries(case, bridge):
    basic = bridge.elements_page(type_exact="goc", include="basic", limit=50)
    case.assertGreater(basic.count, 1)
    for element in basic.elements:
        case.assertNotIn("properties", element.raw)
    time.sleep(0.05)
    rich = bridge.elements_page(type_exact="goc", include="basic,bounds,properties", limit=50)
    case.assertGreater(rich.scene_sequence, basic.scene_sequence)
    case.assertEqual([e.id for e in basic.elements], [e.id for e in rich.elements])
    for before, after in zip(basic.elements, rich.elements):
        for key in ("logical_id", "instance_generation", "created_scene_sequence"):
            case.assertEqual(before.raw[key], after.raw[key], (key, after.raw))
        properties = after.raw["properties"]
        case.assertEqual(after.type, properties["type"])
        case.assertIsInstance(properties["position"], list)
        case.assertEqual(3, len(properties["position"]))
        case.assertTrue(all(math.isfinite(v) for v in properties["position"]))

    # Components sharing one instance must retain the first observed creation
    # sequence. The index must not confuse the component's path with its owner.
    owner = rich.elements[0]
    children = bridge.elements(logical_id=owner.logical_id, include="basic,properties", limit=500)
    case.assertGreater(len(children), 1)
    for child in children:
        case.assertEqual(owner.raw["created_scene_sequence"], child.raw["created_scene_sequence"])

    page = bridge.elements_page(type_exact="goc", include="basic", limit=1, offset=1)
    case.assertEqual(rich.elements[1].id, page.elements[0].id)
    case.assertEqual(rich.matched, page.matched)

    # A text property must remain a string, even when it is numeric-looking.
    texts = bridge.elements(type="text", include="basic,properties", limit=500)
    case.assertGreater(len(texts), 0)
    for element in texts:
        properties = element.raw["properties"]
        for key in ("enabled", "visible"):
            if key in properties:
                case.assertIs(type(properties[key]), bool)
        for key in ("position", "scale", "rotation", "color"):
            if key in properties:
                case.assertIn(len(properties[key]), (3, 4))
                case.assertTrue(all(math.isfinite(v) for v in properties[key]))
        if "text" in element.raw["properties"]:
            case.assertIsInstance(element.raw["properties"]["text"], str)
            if element.text:
                case.assertEqual(element.text, element.raw["properties"]["text"])
