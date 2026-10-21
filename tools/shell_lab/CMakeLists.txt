# Runs the Apple app shell on its own: data status, device budget and live lifecycle events.
add_executable(shell_lab main.cpp)
target_link_libraries(shell_lab PRIVATE anomaly_apple_shell)

# Final game bundle id, so the game data pushed to the app container is shared with the real game.
anomaly_configure_game_app(shell_lab DISPLAY_NAME "Anomaly Shell Lab")
