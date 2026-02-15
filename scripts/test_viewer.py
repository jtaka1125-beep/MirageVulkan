#!/usr/bin/env python3
"""Simple test viewer"""
import tkinter as tk
from tkinter import ttk

root = tk.Tk()
root.title("Test Viewer")
root.geometry("800x600")
root.configure(bg='#2d2d2d')

# Title
label = tk.Label(root, text="MirageTestKit Test Viewer",
                 fg='white', bg='#3d3d3d', font=('Arial', 20))
label.pack(fill='x', pady=20)

# Info
info = tk.Label(root, text="If you can see this, tkinter works!",
                fg='lime', bg='#2d2d2d', font=('Arial', 14))
info.pack(pady=20)

# Devices
frame = tk.Frame(root, bg='#1d1d1d')
frame.pack(fill='both', expand=True, padx=20, pady=20)

for i, name in enumerate(["Device 1", "Device 2", "Device 3"]):
    box = tk.Frame(frame, bg='#4d4d4d', width=200, height=150)
    box.pack(side='left', padx=10, pady=10)
    box.pack_propagate(False)
    tk.Label(box, text=name, fg='white', bg='#4d4d4d', font=('Arial', 12)).pack(pady=10)
    tk.Label(box, text="Port: 5000" + str(i), fg='cyan', bg='#4d4d4d').pack()

# Status
status = tk.Label(root, text="Status: Ready", fg='#aaa', bg='#1d1d1d', anchor='w')
status.pack(fill='x', side='bottom')

print("Window created - should be visible now")
root.mainloop()
