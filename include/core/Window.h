#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/Sector.h"


namespace core
{

	class Window : public Object
	{
	public:

		enum struct State
		{
			Open,
			Opening,
			Closed,
			Closing,
			Broken,
			Frosted,
			Frosting,
			Unfrosting,
			Tinted,
			Tinting,
			Untinting
		};

		enum struct Style
		{
			Clear,
			Tinted,
			Frosted
		};

	private:

		uint32_t mCellsWide, mDecksHigh; 

		State mState;

		Style mStyle;

		std::shared_ptr<const Sector> mSectors[2];

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Window(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, std::shared_ptr<const Sector> sectors[2]);

		~Window() = default;

		[[nodiscard]] uint32_t getCellsWide() const;

		[[nodiscard]] uint32_t getDecksHigh() const;

		[[nodiscard]] State const& getState() const;

		[[nodiscard]] Style getStyle() const;

		[[nodiscard]] std::shared_ptr<const Sector> getSector(uint32_t layerIndex) const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;
	};

} // core
