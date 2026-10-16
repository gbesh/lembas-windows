"""The four Lembas looks for Windows.

The Windhawk styles are written once with a light palette (Red Book: parchment and red ink) and a
dark one (Starlit Night: midnight and gold). Each look is made by swapping colours in that text:
Shire Morning swaps the light colours for sage and green ink, Elven Silver swaps the dark ones for
black-blue and silver. Light mode shows the light colours and dark mode the dark ones, so each look
only needs its own half.
"""

PHASES = ["shire", "redbook", "starlit", "elven"]

LOOKS = {
    "shire": {
        "name": "Shire Morning", "light": 1, "wallpaper": "Lembas - Shire Morning.jpg",
        # lightest -> darkest; Windows uses the darker shades in light mode
        "accent": ["DCEBCF", "BCD7A6", "93B977", "6A9650", "4E7A3A", "3B5E2B", "29421E", "182A11"],
        "swap": {
            "F3E8CD": "EDF0DC", "F5EBD2": "F0F3E1", "F3E4C0": "E6ECCE", "FFF9EA": "FAFCF0",
            "FBF4E0": "F6F9EA", "FFF8E6": "FAFCEE", "7A5A34": "4F6B3A", "9A2A1C": "3F7A34",
            "B8892F": "4E7A3A", "E2C46F": "93B977", "C9A04C": "5E8C46", "home-light": "home-shire",
        },
    },
    "redbook": {
        "name": "Red Book", "light": 1, "wallpaper": "Lembas - Red Book.jpg",
        "accent": ["F2D3CB", "E3A79A", "C86E5F", "A8392A", "8E2A1D", "6E2015", "4E160E", "2E0C07"],
        "swap": {"B8892F": "9A2A1C", "E2C46F": "C86E5F", "C9A04C": "A8392A", "home-light": "home-redbook"},
    },
    "starlit": {
        "name": "Starlit Night", "light": 0, "wallpaper": "Lembas - Starlit Night.jpg",
        "accent": ["F7E9C1", "EDD596", "DDBD6D", "C9A04C", "A8822F", "85651F", "5E4612", "3A2A0A"],
        "swap": {"home-dark": "home-starlit"},
    },
    "elven": {
        "name": "Elven Silver", "light": 0, "wallpaper": "Lembas - Elven Silver.jpg",
        "accent": ["EEF4F7", "D6E3EA", "B9CEDA", "9DB8C7", "7E9DAE", "60808F", "445D69", "2A3B43"],
        "swap": {
            "0B1020": "0A0E11", "121628": "13181C", "0A0E1A": "090C0F", "0E1430": "0F1519",
            "141A30": "151C21", "141C3E": "18212A", "1F2A52": "26323C", "D9BC7A": "C9D7DF",
            "B8892F": "7E9DAE", "E2C46F": "D6E3EA", "C9A04C": "9DB8C7", "4A4232": "2E363C",
            "home-dark": "home-elven",
        },
    },
}


def apply(text, phase):
    for old, new in LOOKS[phase]["swap"].items():
        text = text.replace(old, new)
    return text
