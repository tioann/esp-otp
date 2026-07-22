import pyotp

# otpauth://totp/MyApp:user%40example.com?secret=U7MQO34PR6ZIF2LRW5ZUWMDSGTYSYT3V&issuer=MyApp&algorithm=SHA1&digits=6&period=30


# Store this secret securely for each user
secret = "U7MQO34PR6ZIF2LRW5ZUWMDSGTYSYT3V"

# Generate current TOTP code
totp = pyotp.TOTP(secret)
print(totp.now())  # e.g., "492039"
