local prometheum = require("prometheum.v1")

local WAIT_TICKS = 5 * 60

return {
  api_version = prometheum.api_version,
  factory = function(configuration)
    local heading_to_first_marker = true

    local function move_to_next_marker(context)
      local destination = heading_to_first_marker
        and configuration.first_marker
        or configuration.second_marker
      local result = context.move_to(destination)
      if not result.accepted then
        error("could not move to the next Marker: " .. result.status)
      end
    end

    return {
      on_start = function(context)
        move_to_next_marker(context)
      end,

      on_event = function(event, context)
        if event.type ~= "destination_reached" then
          return
        end

        heading_to_first_marker = not heading_to_first_marker
        context.set_timer("marker_wait", WAIT_TICKS)
      end,

      on_timer = function(name, context)
        if name == "marker_wait" then
          move_to_next_marker(context)
        end
      end,
    }
  end,
}
