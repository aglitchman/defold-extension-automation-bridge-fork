#include "automation_bridge_private.h"
#include "automation_bridge_defold_private_api.h"

#if defined(DM_DEBUG)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <dmsdk/dlib/profile.h>

namespace dmAutomationBridge
{
    static const char* SceneNodeTypeToString(dmGameObject::SceneNodeType type)
    {
        switch (type)
        {
        case dmGameObject::SCENE_NODE_TYPE_COLLECTION:
            return "collectionc";
        case dmGameObject::SCENE_NODE_TYPE_GAMEOBJECT:
            return "goc";
        case dmGameObject::SCENE_NODE_TYPE_COMPONENT:
            return "component";
        case dmGameObject::SCENE_NODE_TYPE_SUBCOMPONENT:
            return "subcomponent";
        default:
            return "element";
        }
    }

    static const char* KindFromType(dmGameObject::SceneNodeType scene_node_type, const char* type)
    {
        if (StartsWith(type, "gui_node_"))
        {
            return "gui_node";
        }
        if (StringsEqual(type, "goc") || scene_node_type == dmGameObject::SCENE_NODE_TYPE_GAMEOBJECT)
        {
            return "game_object";
        }
        if (StringsEqual(type, "collectionc") || scene_node_type == dmGameObject::SCENE_NODE_TYPE_COLLECTION)
        {
            return "collection";
        }
        if (scene_node_type == dmGameObject::SCENE_NODE_TYPE_COMPONENT)
        {
            return "component";
        }
        if (scene_node_type == dmGameObject::SCENE_NODE_TYPE_SUBCOMPONENT)
        {
            return "subcomponent";
        }
        return "element";
    }

    static void SetAnchorFromPivot(Node* node, const char* pivot)
    {
        if (StringsEqual(pivot, "PIVOT_NW"))
        {
            node->m_AnchorX = 0.0f;
            node->m_AnchorY = 0.0f;
        }
        else if (StringsEqual(pivot, "PIVOT_N"))
        {
            node->m_AnchorX = 0.5f;
            node->m_AnchorY = 0.0f;
        }
        else if (StringsEqual(pivot, "PIVOT_NE"))
        {
            node->m_AnchorX = 1.0f;
            node->m_AnchorY = 0.0f;
        }
        else if (StringsEqual(pivot, "PIVOT_W"))
        {
            node->m_AnchorX = 0.0f;
            node->m_AnchorY = 0.5f;
        }
        else if (StringsEqual(pivot, "PIVOT_CENTER"))
        {
            node->m_AnchorX = 0.5f;
            node->m_AnchorY = 0.5f;
        }
        else if (StringsEqual(pivot, "PIVOT_E"))
        {
            node->m_AnchorX = 1.0f;
            node->m_AnchorY = 0.5f;
        }
        else if (StringsEqual(pivot, "PIVOT_SW"))
        {
            node->m_AnchorX = 0.0f;
            node->m_AnchorY = 1.0f;
        }
        else if (StringsEqual(pivot, "PIVOT_S"))
        {
            node->m_AnchorX = 0.5f;
            node->m_AnchorY = 1.0f;
        }
        else if (StringsEqual(pivot, "PIVOT_SE"))
        {
            node->m_AnchorX = 1.0f;
            node->m_AnchorY = 1.0f;
        }
    }

    static void FreeProperty(Property* property)
    {
        FreeString(&property->m_StringValue);
    }

    static bool MakeProperty(Property* property, const dmGameObject::SceneNodeProperty* source)
    {
        memset(property, 0, sizeof(*property));
        property->m_NameHash = source->m_NameHash;

        switch (source->m_Type)
        {
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_HASH:
            if (!SetHashString(&property->m_StringValue, source->m_Value.m_Hash))
            {
                return false;
            }
            break;
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_NUMBER:
        {
            property->m_Number = source->m_Value.m_Number;
            property->m_HasNumber = true;
            break;
        }
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_BOOLEAN:
            property->m_BoolValue = source->m_Value.m_Bool;
            property->m_HasBool = true;
            SetString(&property->m_StringValue, source->m_Value.m_Bool ? "true" : "false");
            break;
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_URL:
            SetString(&property->m_StringValue, source->m_Value.m_URL);
            break;
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_TEXT:
            SetString(&property->m_StringValue, source->m_Value.m_Text ? source->m_Value.m_Text : "");
            break;
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_VECTOR3:
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_VECTOR4:
        case dmGameObject::SCENE_NODE_PROPERTY_TYPE_QUAT:
        {
            property->m_HasVector = true;
            property->m_VectorCount = source->m_Type == dmGameObject::SCENE_NODE_PROPERTY_TYPE_VECTOR3 ? 3 : 4;

            for (uint32_t i = 0; i < property->m_VectorCount; ++i)
            {
                property->m_Vector[i] = source->m_Value.m_V4[i];
            }
            break;
        }
        default:
            break;
        }

        return true;
    }

    // Own the sampled values, but format only properties actually returned.
    // This also supports a richer include on the same cached frame snapshot.
    static void AppendPropertyJson(StringBuffer* out, const Property* property)
    {
        if (property->m_HasNumber)
        {
            AppendNumber(out, property->m_Number);
        }
        else if (property->m_HasBool)
        {
            StringBufferAppend(out, property->m_BoolValue ? "true" : "false");
        }
        else if (property->m_HasVector)
        {
            StringBufferAppendChar(out, '[');
            for (uint32_t i = 0; i < property->m_VectorCount; ++i)
            {
                if (i > 0) StringBufferAppendChar(out, ',');
                AppendNumber(out, property->m_Vector[i]);
            }
            StringBufferAppendChar(out, ']');
        }
        else if (property->m_StringValue)
        {
            AppendJsonString(out, property->m_StringValue);
        }
        else
        {
            StringBufferAppend(out, "null");
        }
    }

    static void InitNode(Node* node)
    {
        memset(node, 0, sizeof(*node));
        node->m_Visible = true;
        node->m_Enabled = true;
        node->m_AnchorX = 0.5f;
        node->m_AnchorY = 0.5f;
    }

    static void FreeNode(Node* node)
    {
        FreeString(&node->m_Id);
        FreeString(&node->m_InstanceId);
        FreeString(&node->m_LogicalId);
        FreeString(&node->m_Name);
        FreeString(&node->m_Type);
        FreeString(&node->m_Kind);
        FreeString(&node->m_Path);
        FreeString(&node->m_Parent);
        FreeString(&node->m_Text);
        FreeString(&node->m_Url);
        FreeString(&node->m_Resource);
        FreeString(&node->m_AutomationId);
        FreeString(&node->m_LocalizationKey);
        FreeString(&node->m_Role);
        for (uint32_t i = 0; i < node->m_Properties.m_Count; ++i)
        {
            FreeProperty(&node->m_Properties.m_Data[i]);
        }
        ArrayFree(&node->m_Properties);
        ArrayFree(&node->m_Children);
    }

    static void ApplyPropertyToNode(Node* node, const Property* property)
    {
        static const dmhash_t ID_HASH = dmHashString64("id");
        static const dmhash_t TYPE_HASH = dmHashString64("type");
        static const dmhash_t TEXT_HASH = dmHashString64("text");
        static const dmhash_t URL_HASH = dmHashString64("url");
        static const dmhash_t RESOURCE_HASH = dmHashString64("resource");
        static const dmhash_t VISIBLE_HASH = dmHashString64("visible");
        static const dmhash_t ENABLED_HASH = dmHashString64("enabled");
        static const dmhash_t PIVOT_HASH = dmHashString64("pivot");
        static const dmhash_t WORLD_POSITION_HASH = dmHashString64("world_position");
        static const dmhash_t POSITION_HASH = dmHashString64("position");
        static const dmhash_t WORLD_SIZE_HASH = dmHashString64("world_size");
        static const dmhash_t SIZE_HASH = dmHashString64("size");
        if ((property->m_NameHash == ID_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetString(&node->m_Name, property->m_StringValue);
        }
        else if ((property->m_NameHash == TYPE_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetString(&node->m_Type, property->m_StringValue);
        }
        else if ((property->m_NameHash == TEXT_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetString(&node->m_Text, property->m_StringValue);
        }
        else if ((property->m_NameHash == URL_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetString(&node->m_Url, property->m_StringValue);
        }
        else if ((property->m_NameHash == RESOURCE_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetString(&node->m_Resource, property->m_StringValue);
        }
        else if ((property->m_NameHash == VISIBLE_HASH) && property->m_HasBool)
        {
            node->m_Visible = property->m_BoolValue;
        }
        else if ((property->m_NameHash == ENABLED_HASH) && property->m_HasBool)
        {
            node->m_Enabled = property->m_BoolValue;
        }
        else if ((property->m_NameHash == PIVOT_HASH) && !IsEmpty(property->m_StringValue))
        {
            SetAnchorFromPivot(node, property->m_StringValue);
        }

        if (property->m_HasVector && property->m_VectorCount >= 2)
        {
            if ((property->m_NameHash == WORLD_POSITION_HASH) || (!node->m_HasPosition && (property->m_NameHash == POSITION_HASH)))
            {
                node->m_HasPosition = true;
                node->m_Position[0] = property->m_Vector[0];
                node->m_Position[1] = property->m_Vector[1];
                node->m_Position[2] = property->m_VectorCount >= 3 ? property->m_Vector[2] : 0.0f;
            }
            else if ((property->m_NameHash == WORLD_SIZE_HASH) || (!node->m_HasSize && (property->m_NameHash == SIZE_HASH)))
            {
                node->m_HasSize = true;
                node->m_Size[0] = property->m_Vector[0];
                node->m_Size[1] = property->m_Vector[1];
                node->m_Size[2] = property->m_VectorCount >= 3 ? property->m_Vector[2] : 0.0f;
            }
        }
    }

    static void ComputeFallbackBounds(Node* node, uint32_t screen_w, uint32_t screen_h, uint32_t display_w, uint32_t display_h)
    {
        if (!node->m_HasPosition || screen_w == 0 || screen_h == 0)
        {
            return;
        }

        float width = node->m_HasSize ? fabsf(node->m_Size[0]) : 0.0f;
        float height = node->m_HasSize ? fabsf(node->m_Size[1]) : 0.0f;
        float x = node->m_Position[0];
        float y = node->m_Position[1];

        if (!IsFiniteFloat(x) || !IsFiniteFloat(y) || !IsFiniteFloat(width) || !IsFiniteFloat(height))
        {
            return;
        }

        if (StringsEqual(node->m_Kind, "gui_node"))
        {
            node->m_Bounds.m_X = x - width * node->m_AnchorX;
            node->m_Bounds.m_Y = (float)screen_h - (y + height * node->m_AnchorY);
            node->m_Bounds.m_W = width;
            node->m_Bounds.m_H = height;
            node->m_Bounds.m_CX = node->m_Bounds.m_X + width * 0.5f;
            node->m_Bounds.m_CY = node->m_Bounds.m_Y + height * 0.5f;
        }
        else
        {
            float source_w = display_w > 0 ? (float)display_w : (float)screen_w;
            float source_h = display_h > 0 ? (float)display_h : (float)screen_h;
            float scale_x = source_w > 0.0f ? (float)screen_w / source_w : 1.0f;
            float scale_y = source_h > 0.0f ? (float)screen_h / source_h : 1.0f;
            node->m_Bounds.m_CX = x * scale_x;
            node->m_Bounds.m_CY = (source_h - y) * scale_y;
            node->m_Bounds.m_W = width * scale_x;
            node->m_Bounds.m_H = height * scale_y;
            node->m_Bounds.m_X = node->m_Bounds.m_CX - node->m_Bounds.m_W * 0.5f;
            node->m_Bounds.m_Y = node->m_Bounds.m_CY - node->m_Bounds.m_H * 0.5f;
        }

        node->m_Bounds.m_NX = node->m_Bounds.m_CX / (float)screen_w;
        node->m_Bounds.m_NY = node->m_Bounds.m_CY / (float)screen_h;
        node->m_Bounds.m_Valid = true;
    }

    static bool IsUsableBounds(const Bounds* bounds)
    {
        return bounds->m_Valid &&
               IsFiniteFloat(bounds->m_X) &&
               IsFiniteFloat(bounds->m_Y) &&
               IsFiniteFloat(bounds->m_W) &&
               IsFiniteFloat(bounds->m_H) &&
               bounds->m_W >= 0.0f &&
               bounds->m_H >= 0.0f;
    }

    static bool HasClickableArea(const Bounds* bounds)
    {
        return IsUsableBounds(bounds) && bounds->m_W > 0.5f && bounds->m_H > 0.5f;
    }

    static bool ShouldUseChildBounds(const Node* node)
    {
        return !HasClickableArea(&node->m_Bounds);
    }

    static bool ChildCanContributeBounds(const Node* child)
    {
        return child->m_Visible && child->m_Enabled && HasClickableArea(&child->m_Bounds);
    }

    static void UseChildBoundsIfBetter(Snapshot* snapshot, uint32_t index)
    {
        if (!snapshot || index >= snapshot->m_Nodes.m_Count)
        {
            return;
        }

        Node* node = &snapshot->m_Nodes.m_Data[index];
        if (!ShouldUseChildBounds(node))
        {
            return;
        }

        float min_x = 0.0f;
        float min_y = 0.0f;
        float max_x = 0.0f;
        float max_y = 0.0f;
        bool has_bounds = false;

        for (uint32_t i = 0; i < node->m_Children.m_Count; ++i)
        {
            uint32_t child_index = node->m_Children.m_Data[i];
            if (child_index >= snapshot->m_Nodes.m_Count)
            {
                continue;
            }

            const Node* child = &snapshot->m_Nodes.m_Data[child_index];
            if (!ChildCanContributeBounds(child))
            {
                continue;
            }

            const Bounds* bounds = &child->m_Bounds;
            float child_min_x = bounds->m_X;
            float child_min_y = bounds->m_Y;
            float child_max_x = bounds->m_X + bounds->m_W;
            float child_max_y = bounds->m_Y + bounds->m_H;

            if (!has_bounds)
            {
                min_x = child_min_x;
                min_y = child_min_y;
                max_x = child_max_x;
                max_y = child_max_y;
                has_bounds = true;
            }
            else
            {
                min_x = MinFloat(min_x, child_min_x);
                min_y = MinFloat(min_y, child_min_y);
                max_x = MaxFloat(max_x, child_max_x);
                max_y = MaxFloat(max_y, child_max_y);
            }
        }

        if (!has_bounds)
        {
            return;
        }

        node = &snapshot->m_Nodes.m_Data[index];
        node->m_Bounds.m_X = min_x;
        node->m_Bounds.m_Y = min_y;
        node->m_Bounds.m_W = MaxFloat(0.0f, max_x - min_x);
        node->m_Bounds.m_H = MaxFloat(0.0f, max_y - min_y);
        node->m_Bounds.m_CX = min_x + node->m_Bounds.m_W * 0.5f;
        node->m_Bounds.m_CY = min_y + node->m_Bounds.m_H * 0.5f;
        node->m_Bounds.m_NX = snapshot->m_WindowWidth > 0 ? node->m_Bounds.m_CX / (float)snapshot->m_WindowWidth : 0.0f;
        node->m_Bounds.m_NY = snapshot->m_WindowHeight > 0 ? node->m_Bounds.m_CY / (float)snapshot->m_WindowHeight : 0.0f;
        node->m_Bounds.m_Valid = true;
    }

    struct InstanceIdentity
    {
        const char* m_Id;
        uint32_t m_Generation;

        bool operator==(const InstanceIdentity& other) const
        {
            return m_Generation == other.m_Generation && StringsEqual(m_Id, other.m_Id);
        }
    };

    struct InstanceIdentityHash
    {
        size_t operator()(const InstanceIdentity& identity) const
        {
            return (size_t)(dmHashString64(identity.m_Id) ^ ((uint64_t)identity.m_Generation << 32));
        }
    };

    typedef std::unordered_map<InstanceIdentity, uint64_t, InstanceIdentityHash> IdentityIndex;

    // Keys borrow strings from the previous snapshot, which stays alive for the
    // whole build. emplace preserves the first match when several components
    // share an instance, exactly like the old linear lookup.
    static void IndexPreviousIdentities(const Snapshot* previous, IdentityIndex* identities)
    {
        DM_PROFILE("BridgeSnapshotIdentityIndex");
        identities->reserve(previous->m_Nodes.m_Count);
        for (uint32_t i = 0; i < previous->m_Nodes.m_Count; ++i)
        {
            const Node* node = &previous->m_Nodes.m_Data[i];
            if (node->m_HasInstanceIdentity)
            {
                InstanceIdentity key = {node->m_InstanceId, node->m_InstanceGeneration};
                identities->emplace(key, node->m_CreatedSceneSequence);
            }
        }
    }

    void InitSnapshot(Snapshot* snapshot)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->m_Root = -1;
    }

    void FreeSnapshot(Snapshot* snapshot)
    {
        for (uint32_t i = 0; i < snapshot->m_Nodes.m_Count; ++i)
        {
            FreeNode(&snapshot->m_Nodes.m_Data[i]);
        }
        ArrayFree(&snapshot->m_Nodes);
        InitSnapshot(snapshot);
    }

    static int32_t BuildNode(Snapshot* snapshot, Array<dmGameObject::SceneNode>* native_nodes, const IdentityIndex& identities, dmGameObject::SceneNode* scene_node, int32_t parent_index, const char* parent_path, uint32_t sibling_index)
    {
        Node node;
        InitNode(&node);

        dmGameObject::SceneNodePropertyIterator pit = dmGameObject::TraverseIterateProperties(scene_node);
        while (dmGameObject::TraverseIteratePropertiesNext(&pit))
        {
            Property property;
            if (MakeProperty(&property, &pit.m_Property))
            {
                ApplyPropertyToNode(&node, &property);
                if (!ArrayPush(&node.m_Properties, &property))
                {
                    FreeProperty(&property);
                }
            }
        }

        // Camera selection retains the previous depth-first order and uses
        // native properties before annotations. Bounds are resolved afterwards.
        if (scene_node->m_Type == dmGameObject::SCENE_NODE_TYPE_COMPONENT && node.m_Enabled && StringsEqual(node.m_Type, "camerac"))
        {
            DefoldPrivateApiUseSnapshotCamera(scene_node);
        }

        if (IsEmpty(node.m_Type))
        {
            SetString(&node.m_Type, SceneNodeTypeToString(scene_node->m_Type));
        }
        SetString(&node.m_Kind, KindFromType(scene_node->m_Type, node.m_Type));

        if (IsEmpty(node.m_Name))
        {
            char buffer[64];
            dmSnPrintf(buffer, sizeof(buffer), "%s_%u", node.m_Type, sibling_index);
            SetString(&node.m_Name, buffer);
        }

        ApplyNodeAnnotation(&node);

        const char* safe_segment = IsEmpty(node.m_Name) ? node.m_Type : node.m_Name;
        char segment[128];
        dmSnPrintf(segment, sizeof(segment), "%s[%u]", safe_segment, sibling_index);

        StringBuffer path;
        StringBufferInit(&path);
        if (IsEmpty(parent_path))
        {
            StringBufferAppendChar(&path, '/');
            StringBufferAppend(&path, segment);
        }
        else
        {
            StringBufferAppend(&path, parent_path);
            StringBufferAppendChar(&path, '/');
            StringBufferAppend(&path, segment);
        }
        node.m_Path = StringBufferDetach(&path);

        uint64_t id_hash = dmHashBuffer64(node.m_Path, (uint32_t)strlen(node.m_Path));
        char id[32];
        dmSnPrintf(id, sizeof(id), "e:%016llx", (unsigned long long)id_hash);
        SetString(&node.m_Id, id);

        // Collection roots only initialize m_Collection. The remaining union-like
        // SceneNode fields are unspecified, so m_Instance must only be read for
        // Scene element kinds that Defold documents as carrying an instance.
        if (scene_node->m_Type != dmGameObject::SCENE_NODE_TYPE_COLLECTION && scene_node->m_Instance)
        {
            dmhash_t instance_identifier = dmGameObject::GetIdentifier(scene_node->m_Instance);
            node.m_InstanceGeneration = dmGameObject::GetGeneration(scene_node->m_Instance);
            node.m_HasInstanceIdentity = true;
            SetHashString(&node.m_InstanceId, instance_identifier);

            char logical_id[96];
            dmSnPrintf(logical_id, sizeof(logical_id), "instance:%016llx:g%u",
                       (unsigned long long)instance_identifier,
                       node.m_InstanceGeneration);
            SetString(&node.m_LogicalId, logical_id);
            node.m_CreatedSceneSequence = snapshot->m_Sequence;

            InstanceIdentity key = {node.m_InstanceId, node.m_InstanceGeneration};
            IdentityIndex::const_iterator old = identities.find(key);
            if (old != identities.end())
            {
                node.m_CreatedSceneSequence = old->second;
            }
        }

        if (parent_index >= 0 && (uint32_t)parent_index < snapshot->m_Nodes.m_Count)
        {
            SetString(&node.m_Parent, snapshot->m_Nodes.m_Data[parent_index].m_Id);
        }

        uint32_t index = snapshot->m_Nodes.m_Count;
        // Keep native handles only until this synchronous snapshot build ends.
        if (!ArrayReserve(native_nodes, index + 1) || !ArrayPush(&snapshot->m_Nodes, &node))
        {
            FreeNode(&node);
            return -1;
        }

        ArrayPush(native_nodes, scene_node);

        dmGameObject::SceneNodeIterator it = dmGameObject::TraverseIterateChildren(scene_node);
        uint32_t child_index = 0;
        while (dmGameObject::TraverseIterateNext(&it))
        {
            int32_t child = BuildNode(snapshot, native_nodes, identities, &it.m_Node, (int32_t)index, snapshot->m_Nodes.m_Data[index].m_Path, child_index++);
            if (child >= 0)
            {
                uint32_t child_u32 = (uint32_t)child;
                ArrayPush(&snapshot->m_Nodes.m_Data[index].m_Children, &child_u32);
            }
        }

        return (int32_t)index;
    }

    static void RefreshScreenInfo(Snapshot* snapshot)
    {
        snapshot->m_DisplayWidth = g_AutomationBridge.m_DisplayWidth;
        snapshot->m_DisplayHeight = g_AutomationBridge.m_DisplayHeight;

        if (!g_AutomationBridge.m_GraphicsContext)
        {
            return;
        }

        snapshot->m_WindowWidth = dmGraphics::GetWindowWidth(g_AutomationBridge.m_GraphicsContext);
        snapshot->m_WindowHeight = dmGraphics::GetWindowHeight(g_AutomationBridge.m_GraphicsContext);
        // The public window size is the drawable framebuffer size on Defold's
        // graphics backends. GetWidth/GetHeight are the configured project
        // reference size and must not be mislabeled as backbuffer pixels.
        snapshot->m_BackbufferWidth = snapshot->m_WindowWidth;
        snapshot->m_BackbufferHeight = snapshot->m_WindowHeight;
        snapshot->m_DisplayScale = dmGraphics::GetDisplayScaleFactor(g_AutomationBridge.m_GraphicsContext);
        if (!IsFiniteFloat(snapshot->m_DisplayScale) || snapshot->m_DisplayScale <= 0.0f)
        {
            snapshot->m_DisplayScale = 1.0f;
        }
        dmGraphics::GetViewport(g_AutomationBridge.m_GraphicsContext, &snapshot->m_ViewportX, &snapshot->m_ViewportY, &snapshot->m_ViewportWidth, &snapshot->m_ViewportHeight);

        if (snapshot->m_WindowWidth == 0)
        {
            snapshot->m_WindowWidth = snapshot->m_ViewportWidth;
        }
        if (snapshot->m_WindowHeight == 0)
        {
            snapshot->m_WindowHeight = snapshot->m_ViewportHeight;
        }
    }

    void UpdateSnapshot()
    {
        DM_PROFILE("BridgeSnapshot");
        Snapshot snapshot;
        InitSnapshot(&snapshot);
        snapshot.m_Sequence = g_AutomationBridge.m_Snapshot.m_Sequence + 1;
        RefreshScreenInfo(&snapshot);

        if (g_AutomationBridge.m_Register)
        {
            dmGameObject::SceneNode root;
            memset(&root, 0, sizeof(root));
            if (dmGameObject::TraverseGetRoot(g_AutomationBridge.m_Register, &root))
            {
                IdentityIndex identities;
                IndexPreviousIdentities(&g_AutomationBridge.m_Snapshot, &identities);
                Array<dmGameObject::SceneNode> native_nodes = {};
                DefoldPrivateApiResetSnapshotCamera();
                {
                    DM_PROFILE("BridgeSnapshotTraverse");
                    snapshot.m_Root = BuildNode(&snapshot, &native_nodes, identities, &root, -1, "", 0);
                }
                {
                    DM_PROFILE("BridgeSnapshotBounds");
                    // Children follow parents in traversal order. Reverse order
                    // resolves their bounds before combining bounds for parents.
                    for (uint32_t i = snapshot.m_Nodes.m_Count; i > 0; --i)
                    {
                        Node* node = &snapshot.m_Nodes.m_Data[i - 1];
                        Bounds bounds;
                        if (DefoldPrivateApiComputeBounds(&native_nodes.m_Data[i - 1], node, &snapshot, &bounds))
                        {
                            node->m_Bounds = bounds;
                        }
                        else
                        {
                            ComputeFallbackBounds(node, snapshot.m_WindowWidth, snapshot.m_WindowHeight,
                                                  snapshot.m_DisplayWidth, snapshot.m_DisplayHeight);
                        }
                        UseChildBoundsIfBetter(&snapshot, i - 1);
                    }
                }
                ArrayFree(&native_nodes);
            }
        }

        FreeSnapshot(&g_AutomationBridge.m_Snapshot);
        g_AutomationBridge.m_Snapshot = snapshot;
    }

    const Node* FindNodeById(const char* id)
    {
        const Snapshot* snapshot = &g_AutomationBridge.m_Snapshot;
        for (uint32_t i = 0; i < snapshot->m_Nodes.m_Count; ++i)
        {
            if (StringsEqual(snapshot->m_Nodes.m_Data[i].m_Id, id))
            {
                return &snapshot->m_Nodes.m_Data[i];
            }
        }
        return 0;
    }

    static void AppendBoundsJson(StringBuffer* out, const Bounds* bounds)
    {
        if (!bounds->m_Valid)
        {
            StringBufferAppend(out, "null");
            return;
        }

        StringBufferAppend(out, "{\"screen\":{\"x\":");
        AppendNumber(out, bounds->m_X);
        StringBufferAppend(out, ",\"y\":");
        AppendNumber(out, bounds->m_Y);
        StringBufferAppend(out, ",\"w\":");
        AppendNumber(out, bounds->m_W);
        StringBufferAppend(out, ",\"h\":");
        AppendNumber(out, bounds->m_H);
        StringBufferAppend(out, "},\"center\":{\"x\":");
        AppendNumber(out, bounds->m_CX);
        StringBufferAppend(out, ",\"y\":");
        AppendNumber(out, bounds->m_CY);
        StringBufferAppend(out, "},\"normalized\":{\"x\":");
        AppendNumber(out, bounds->m_NX);
        StringBufferAppend(out, ",\"y\":");
        AppendNumber(out, bounds->m_NY);
        StringBufferAppend(out, "}}");
    }

    void AppendNodeJson(StringBuffer* out, const Snapshot* snapshot, const Node* node, const IncludeOptions* include, bool recursive, bool visible_only)
    {
        StringBufferAppend(out, "{\"id\":");
        AppendJsonString(out, node->m_Id);
        StringBufferAppend(out, ",\"snapshot_id\":");
        AppendJsonString(out, node->m_Id);
        StringBufferAppend(out, ",\"scene_sequence\":");
        AppendNumber(out, (double)snapshot->m_Sequence);
        StringBufferAppend(out, ",\"engine_frame\":");
        AppendNumber(out, (double)g_AutomationBridge.m_Frame);
        if (node->m_HasInstanceIdentity)
        {
            StringBufferAppend(out, ",\"instance_id\":");
            AppendJsonString(out, node->m_InstanceId);
            StringBufferAppend(out, ",\"instance_generation\":");
            AppendNumber(out, node->m_InstanceGeneration);
            StringBufferAppend(out, ",\"logical_id\":");
            AppendJsonString(out, node->m_LogicalId);
            StringBufferAppend(out, ",\"created_scene_sequence\":");
            AppendNumber(out, (double)node->m_CreatedSceneSequence);
        }
        StringBufferAppend(out, ",\"name\":");
        AppendJsonString(out, node->m_Name);
        StringBufferAppend(out, ",\"type\":");
        AppendJsonString(out, node->m_Type);
        StringBufferAppend(out, ",\"kind\":");
        AppendJsonString(out, node->m_Kind);
        StringBufferAppend(out, ",\"path\":");
        AppendJsonString(out, node->m_Path);
        StringBufferAppend(out, ",\"parent\":");
        if (IsEmpty(node->m_Parent))
        {
            StringBufferAppend(out, "null");
        }
        else
        {
            AppendJsonString(out, node->m_Parent);
        }
        StringBufferAppend(out, ",\"visible\":");
        StringBufferAppend(out, node->m_Visible ? "true" : "false");
        StringBufferAppend(out, ",\"enabled\":");
        StringBufferAppend(out, node->m_Enabled ? "true" : "false");

        if (!IsEmpty(node->m_Text))
        {
            StringBufferAppend(out, ",\"text\":");
            AppendJsonString(out, node->m_Text);
        }
        if (!IsEmpty(node->m_Url))
        {
            StringBufferAppend(out, ",\"url\":");
            AppendJsonString(out, node->m_Url);
        }
        if (!IsEmpty(node->m_Resource))
        {
            StringBufferAppend(out, ",\"resource\":");
            AppendJsonString(out, node->m_Resource);
        }
        if (!IsEmpty(node->m_AutomationId))
        {
            StringBufferAppend(out, ",\"automation_id\":");
            AppendJsonString(out, node->m_AutomationId);
        }
        if (!IsEmpty(node->m_LocalizationKey))
        {
            StringBufferAppend(out, ",\"localization_key\":");
            AppendJsonString(out, node->m_LocalizationKey);
        }
        if (!IsEmpty(node->m_Role))
        {
            StringBufferAppend(out, ",\"role\":");
            AppendJsonString(out, node->m_Role);
        }

        if (include->m_Bounds)
        {
            StringBufferAppend(out, ",\"bounds\":");
            AppendBoundsJson(out, &node->m_Bounds);
        }

        if (include->m_Properties)
        {
            StringBufferAppend(out, ",\"properties\":{");
            for (uint32_t i = 0; i < node->m_Properties.m_Count; ++i)
            {
                if (i > 0)
                {
                    StringBufferAppendChar(out, ',');
                }
                char* name = 0;
                SetHashString(&name, node->m_Properties.m_Data[i].m_NameHash);
                AppendJsonString(out, name);
                FreeString(&name);
                StringBufferAppendChar(out, ':');
                AppendPropertyJson(out, &node->m_Properties.m_Data[i]);
            }
            StringBufferAppendChar(out, '}');
        }

        if (include->m_Children || recursive)
        {
            StringBufferAppend(out, ",\"children\":[");
            uint32_t emitted_children = 0;
            for (uint32_t i = 0; i < node->m_Children.m_Count; ++i)
            {
                uint32_t child = node->m_Children.m_Data[i];
                if (child < snapshot->m_Nodes.m_Count)
                {
                    const Node* child_node = &snapshot->m_Nodes.m_Data[child];
                    if (visible_only && !child_node->m_Visible)
                    {
                        continue;
                    }
                    if (emitted_children > 0)
                    {
                        StringBufferAppendChar(out, ',');
                    }
                    AppendNodeJson(out, snapshot, child_node, include, recursive, visible_only);
                    ++emitted_children;
                }
            }
            StringBufferAppendChar(out, ']');
        }

        StringBufferAppendChar(out, '}');
    }

    void AppendScreenJson(StringBuffer* out, const Snapshot* snapshot)
    {
        int32_t viewport_top = (int32_t)snapshot->m_BackbufferHeight - snapshot->m_ViewportY - (int32_t)snapshot->m_ViewportHeight;
        StringBufferAppend(out, "{\"window\":{\"x\":0,\"y\":0,\"width\":");
        AppendNumber(out, snapshot->m_WindowWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_WindowHeight);
        StringBufferAppend(out, "},\"client\":{\"x\":0,\"y\":0,\"width\":");
        AppendNumber(out, snapshot->m_WindowWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_WindowHeight);
        StringBufferAppend(out, "},\"backbuffer\":{\"x\":0,\"y\":0,\"width\":");
        AppendNumber(out, snapshot->m_BackbufferWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_BackbufferHeight);
        StringBufferAppend(out, "},\"display\":{\"width\":");
        AppendNumber(out, snapshot->m_DisplayWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_DisplayHeight);
        StringBufferAppend(out, ",\"scale\":");
        AppendNumber(out, snapshot->m_DisplayScale);
        StringBufferAppend(out, "},\"project_content\":{\"x\":0,\"y\":0,\"width\":");
        AppendNumber(out, snapshot->m_DisplayWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_DisplayHeight);
        StringBufferAppend(out, "},\"display_pixels\":{\"x\":0,\"y\":0,\"width\":");
        AppendNumber(out, snapshot->m_WindowWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_WindowHeight);
        StringBufferAppend(out, ",\"scope\":\"window_drawable\"");
        StringBufferAppend(out, "},\"viewport\":{\"x\":");
        AppendNumber(out, snapshot->m_ViewportX);
        StringBufferAppend(out, ",\"y\":");
        AppendNumber(out, viewport_top);
        StringBufferAppend(out, ",\"width\":");
        AppendNumber(out, snapshot->m_ViewportWidth);
        StringBufferAppend(out, ",\"height\":");
        AppendNumber(out, snapshot->m_ViewportHeight);
        StringBufferAppend(out, "},\"display_scale\":");
        AppendNumber(out, snapshot->m_DisplayScale);
        StringBufferAppend(out, ",\"coordinates\":{\"origin\":\"top-left\",\"units\":\"pixels\",\"input_space\":\"window\",\"spaces\":[\"window\",\"client\",\"backbuffer\",\"viewport\",\"normalized_viewport\"]}}");
    }


}

#endif
