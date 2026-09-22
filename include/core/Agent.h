#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "core/SectorPosition.h"
#include "core/Shape.h"
#include "core/Path.h"
#include "core/AgentTag.h"
#include "core/EntityId.h"
#include "core/Serializable.h"


namespace core
{
	class Building;
	class Sector;

	struct PathIterator
	{
		std::shared_ptr<Path> path;
		uint32_t targetNode{ 0 };

		bool atEnd() const
		{
			return !path || targetNode >= (uint32_t)path->nodes.size();
		}
	};

	struct EffectiveAgentColour
	{
		AgentColour value{ EditorDefaultAgentColour };
		// Empty means the editor fallback rather than an inherited property.
		AgentTagId sourceTag{};
	};

	enum class SampledAgentPropertyType
	{
		WalkSpeedModifier
	};

	struct AgentPropertySample
	{
		SampledAgentPropertyType type{ SampledAgentPropertyType::WalkSpeedModifier };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		float value{ 1.0f };

		bool operator==(AgentPropertySample const& other) const = default;
	};

	struct EffectiveAgentWalkSpeedModifier
	{
		float value{ 1.0f };
		// Empty means that base walk speed is unmodified.
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
	};

	class Agent : public Serializable
	{
		friend class Building;
		friend class SimulationCoordinator;
		friend class Sector;

	public:

		struct EdgeTraversalData
		{
			std::shared_ptr<Sector> curSector, nextSector;
			std::shared_ptr<const Vertex> nextVertex;
		};

		enum struct State
		{
			Idle,
			MovingToVertex,
			WaitingForTraversal,
			TraversingEdge,
			AwaitingTraversalCommit
		};

		struct TraversalTask
		{
			TraversalRequestId request;
			TraversalPermitId permit;
			std::shared_ptr<const Edge> edge;
			std::shared_ptr<const Vertex> sourceVertex;
			std::shared_ptr<const Vertex> destinationVertex;
			uint32_t pathNodesConsumed{ 1 };
			uint64_t traversalTicksRemaining{ 0 };
		};

	private:

		std::string mName;

		// The Agent group this Agent is assigned to (ADR 0006). An empty
		// AgentGroupId means no Agent group, which is the default for every
		// newly created Agent and for every Agent loaded from a document that
		// predates the assignment field. The reference is to the group's stable
		// identity, never to its name, so renaming a group rewrites nothing.
		//
		// This is authored editor metadata. It never reaches the runtime
		// snapshot, the simulation events, or any movement, pathfinding,
		// capacity or traversal decision.
		AgentGroupId mAgentGroup{};

		// Agent tag assignments are Building-authored references into the one
		// attached Agent tag registry. A set makes duplicate assignment
		// structurally impossible in memory and gives persistence a stable numeric
		// order. New Agents start with the empty set.
		std::set<AgentTagId> mAgentTags;

		// Modifier samples are authored per-Agent values rather than transient
		// simulation state. Their source identity and property revision make the
		// draw inspectable and let loading distinguish stable data from stale data.
		std::optional<AgentPropertySample> mWalkSpeedModifierSample;

		// Activation is authored state (#118): an activated Agent is simulated,
		// a deactivated one keeps its authored position and route but no tick
		// acts on it. Every Agent starts activated, and so does every Agent
		// loaded from a document that predates the field. The running-simulation
		// gate lives on Building's setAgentActive seam, not here: direct Agent
		// mutation is the unchecked editor seam setFlags already uses.
		bool mActive{ true };

		Building* mBuilding{ nullptr };

		SectorPosition mPosition;

		// Authored position and route are kept separate from transient locomotion.
		// Simulation advances mPosition/mPath, while serialization and Reset use
		// this immutable baseline.
		SectorPosition mResetPosition;
		std::shared_ptr<Path> mResetPath;
		bool mResetPathActive{ false };

		uint32_t mFlags;

		State mState;

		PathIterator mPath;
		// Position before the current route began. Traversal requests use this (or
		// the preceding path node) to retain the side from which the Agent approached.
		Vector2 mPathStartPosition;

		std::optional<TraversalTask> mTraversalTask;
		// A queue request may be made while the preceding same-sector Location
		// edge is still active, allowing the Agent to stop before the queue tail.
		std::optional<TraversalTask> mQueuedTraversalTask;

		// Traversal resources assign local goals; the Agent remains the sole owner
		// of walking and advances itself during the movement phase.
		std::optional<Vector2> mTraversalLocalGoal;

		// Prevent repeated opportunistic presses while following the same immediate
		// Door edge. This is transient locomotion state, not authored simulation data.
		TraversalResourceId mEarlyDoorPressResource;
		bool mEarlyDoorPressAttempted{ false };

		// Set when locomotion stops at the outer edge of an available queue lane.
		// +1 approaches from the left, -1 from the right, and 0 requests at the endpoint.
		int mEarlyQueueApproachDirectionX{ 0 };

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		// Assignment belongs to Building::setAgentGroup, which has already
		// judged both the Agent and the Agent group against this Building.
		void setAgentGroupId(AgentGroupId id) { mAgentGroup = id; }

		// Assignment mutation belongs to Building, which validates the Agent,
		// registry, tag, duplicate state, and paused simulation before calling.
		void assignAgentTag(AgentTagId id) { mAgentTags.insert(id); }
		void removeAgentTag(AgentTagId id) { mAgentTags.erase(id); }
		void setAgentTags(std::set<AgentTagId> tags) { mAgentTags = std::move(tags); }
		void setWalkSpeedModifierSample(AgentPropertySample sample)
		{
			mWalkSpeedModifierSample = sample;
		}
		void clearWalkSpeedModifierSample() { mWalkSpeedModifierSample.reset(); }

		void setPosition(SectorPosition pos, bool authored = true);

		void attachToBuilding(Building* building);

		void assignPath(std::shared_ptr<Path> path, bool startPathing, bool markModified);

		void clearRuntimePath();

		bool moveToPosition(Vector2 const& pos, float frameTime, float speed);

		uint32_t getSkippablePathTarget(uint32_t vertexA) const;

		bool moveToPosition(Vector2 const& pos, float frameTime)
		{
			return moveToPosition(pos, frameTime, getWalkSpeed());
		}

		void startIdling();

		bool nextPathNode();

		void moveToVertex(float frameTime);

		void collectTraversalIntent();

		void allocateTraversal();

		void commitTraversal();

		void cleanupTraversal();

		void considerTraversalReplan();

		float estimateRemainingPathSeconds(std::shared_ptr<Path> const& path, uint32_t fromNode) const;

		void cancelTraversal();

		bool moveToVertexOffset(int dim, float offset, float frameTime);

		// Pathing helpers
		PathNode& getTargetPathNode() const;

		bool atEndOfPath() const;


	public:

		Agent(std::string const& name);

		virtual ~Agent() = default;

		std::string const& getName() const;

		// The Agent group this Agent is assigned to. An empty AgentGroupId
		// means no Agent group.
		AgentGroupId getAgentGroupId() const { return mAgentGroup; }

		// Stable IDs of the Agent tags assigned to this Agent, in ascending
		// numeric order. The referenced definitions live in the Building's
		// attached Agent tag registry.
		std::set<AgentTagId> const& getAgentTagIds() const { return mAgentTags; }
		bool hasAgentTag(AgentTagId id) const { return mAgentTags.contains(id); }

		// Resolves Colour through this Agent's assigned tags. Valid Building state
		// has at most one source; an uncoloured Agent receives the editor default.
		EffectiveAgentColour getEffectiveColour() const;

		// The persisted per-Agent Walk speed draw and its provenance. An Agent
		// without the property exposes the neutral modifier and no source tag.
		EffectiveAgentWalkSpeedModifier getEffectiveWalkSpeedModifier() const;
		std::optional<AgentPropertySample> const& getWalkSpeedModifierSample() const
		{
			return mWalkSpeedModifierSample;
		}

		// Whether this Agent is simulated. Deactivation changes no authored
		// state: position and route stay as they are until an activated tick
		// or a reset works on them.
		bool isActive() const { return mActive; }

		void setActive(bool active);

		State getState() const;

		std::string getDescription() const;

		Sector const* getSector() const;

		Vector2 const& getLocalPosition() const;

		Vector2 getGlobalPosition() const;

		float getWidth() const;

		float getHeight() const;

		Shape getBounds() const;

		float getWalkSpeed() const;

		float getClimbSpeed() const;

		// Used by edge route-cost implementations; this is an observation only.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;

		uint32_t getFlags() const;

		bool flagsSet(uint32_t flags) const;

		void setFlags(uint32_t flags);

		void unsetFlags(uint32_t flags);

		std::shared_ptr<Path> const& getPath() const;

		uint32_t getPathTargetNodeIndex() const;

		bool hasActiveLocomotionTask() const;

		TraversalRequestId getTraversalRequestId() const;

		TraversalPermitId getTraversalPermitId() const;

		EdgeTraversalData getEdgeTraversalData() const;

		void setPath(std::shared_ptr<Path> path, bool startPathing);

		void clearPath();

		void startPathing();

		void pausePathing();

		int chooseVertexOffset(int dim, std::pair<float, uint32_t> const* offsets, uint32_t numOffsets);


		void wake();

		void update(float frameTime);
	};

} // core
